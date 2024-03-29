#include <winsock2.h>
#include <process.h>
#include <WS2tcpip.h>
#include <stdio.h>

#pragma comment (lib, "Ws2_32.lib")

#include "StreamQueue.h"
#include "NPacket.h"
#include "LanServer.h"


CLanServer::CLanServer()
{
	iWorkerThdNum = MAX_THREAD;
	iSessionID = 0;
	iSessionCount = 0;
}

CLanServer::CLanServer(int iWorkerThdNum)
{
	iWorkerThdNum > MAX_THREAD ? MAX_THREAD : iWorkerThdNum;
}

CLanServer::~CLanServer()
{

}

//----------------------------------------------------------------------------------------------------
// LanServer 시작
//----------------------------------------------------------------------------------------------------
BOOL CLanServer::Start(WCHAR *wOpenIP, int iPort, int iWorkerThdNum, BOOL bNagle, int iMaxConnection)
{
	int retval;
	DWORD dwThreadID;

	//////////////////////////////////////////////////////////////////////////////////////////////////
	// 윈속 초기화
	//////////////////////////////////////////////////////////////////////////////////////////////////
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return FALSE;

	//////////////////////////////////////////////////////////////////////////////////////////////////
	// Completion Port 생성
	//////////////////////////////////////////////////////////////////////////////////////////////////
	hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (hIOCP == NULL)
		return FALSE;

	//////////////////////////////////////////////////////////////////////////////////////////////////
	// socket 생성
	//////////////////////////////////////////////////////////////////////////////////////////////////
	listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_sock == INVALID_SOCKET)
		return FALSE;

	//////////////////////////////////////////////////////////////////////////////////////////////////
	//bind
	//////////////////////////////////////////////////////////////////////////////////////////////////
	SOCKADDR_IN serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(iPort);
	InetPton(AF_INET, wOpenIP, &serverAddr.sin_addr);
	retval = bind(listen_sock, (SOCKADDR *)&serverAddr, sizeof(SOCKADDR_IN));
	if (retval == SOCKET_ERROR)
		return FALSE;

	//////////////////////////////////////////////////////////////////////////////////////////////////
	//listen
	//////////////////////////////////////////////////////////////////////////////////////////////////
	retval = listen(listen_sock, SOMAXCONN);
	if (retval == SOCKET_ERROR)
		return FALSE;

	//////////////////////////////////////////////////////////////////////////////////////////////////
	// Thread 생성
	//////////////////////////////////////////////////////////////////////////////////////////////////
	hAcceptThread = (HANDLE)_beginthreadex(
		NULL,
		0,
		AcceptThread,
		this,
		0,
		(unsigned int *)&dwThreadID
		);

	for (int iCnt = 0; iCnt < iWorkerThdNum; iCnt++)
	{
		hWorkerThread[iCnt] = (HANDLE)_beginthreadex(
			NULL,
			0,
			WorkerThread,
			this,
			0,
			(unsigned int *)&dwThreadID
			);
	}

	hMonitorThread = (HANDLE)_beginthreadex(
		NULL,
		0,
		MonitorThread,
		this,
		0,
		(unsigned int *)&dwThreadID
		);

	return TRUE;
}

void CLanServer::Stop()
{

}

int CLanServer::WorkerThread_Update(LPVOID workerArg)
{
	int retval;

	while (1)
	{
		DWORD dwTransferred = 0;
		OVERLAPPED *pOverlapped = NULL;
		CSession *pSession = NULL;

		retval = GetQueuedCompletionStatus(hIOCP, &dwTransferred, (PULONG_PTR)&pSession,
			(LPOVERLAPPED *)&pOverlapped, INFINITE);

		OnWorkerThreadBegin();
		
		/////////////////////////////////////////////////////////////////////////////////////////////
		// Error, 종료 처리
		/////////////////////////////////////////////////////////////////////////////////////////////

		// IOCP 에러 서버 종료
		if (retval == FALSE && (pOverlapped == NULL || pSession == NULL))
		{
			int iErrorCode = WSAGetLastError();
			OnError(iErrorCode, L"IOCP Error\n");

			return -1;
		}

		//워커스레드 정상 종료
		else if (dwTransferred == 0 && pSession == NULL && pOverlapped == NULL)
		{
			OnError(0, L"Worker Thread Done.\n");
			return 0;
		}

		else if (dwTransferred == 0)
		{
			if (retval == FALSE)
				Disconnect(pSession);

			//정상종료
			return 0;
		}
		
		/////////////////////////////////////////////////////////////////////////////////////////////
		// Recv 완료
		// 1. RecvQ Write 위치 옮김
		// 2. 패킷에 받은거 다 넣기
		// 3. 패킷 처리 부분
		// 4. Recv 등록
		// 5. Recv 카운터 + 1
		/////////////////////////////////////////////////////////////////////////////////////////////
		// 여기서 패킷 검증까지 다하고 OnRecv해줘야됨
		// OnRecv에서는 컨텐츠만
		else if (pOverlapped == &pSession->_RecvOverlapped)
		{
			CNPacket *pPacket = CNPacket::Alloc();

			while (1)
			{
				if (!PacketProc(pSession, pPacket))		break;
				OnRecv(pSession->_iSessionID, pPacket);
			}

			pPacket->Free();
			RecvPost(pSession);

			InterlockedAdd((LONG *)&_RecvPacketCounter, dwTransferred / 10);
		}
		
		/////////////////////////////////////////////////////////////////////////////////////////////
		// Send 완료
		// 1. 데이터 온 만큼 SendQ에 ReadPos 이동
		// 2. SendFlag 끔
		// 3. OnSend 호출
		// 4. Send 카운터 + 1
		/////////////////////////////////////////////////////////////////////////////////////////////
		else if (pOverlapped == &pSession->_SendOverlapped)
		{
			pSession->SendQ.RemoveData(dwTransferred);
			pSession->_bSendFlag = FALSE;

			OnSend(pSession->_iSessionID, dwTransferred);
			InterlockedAdd((LONG *)&_SendPacketCounter, dwTransferred / 10);
		}

		//Session Release
		if (0 == InterlockedDecrement((LONG *)&pSession->_lIOCount))
			ReleaseSession(pSession);

		OnWorkerThreadEnd();
	}
	return 0;
}

int CLanServer::AcceptThread_Update(LPVOID acceptArg)
{
	HANDLE retval;

	CSession *pSession;
	int addrlen = sizeof(SOCKADDR_IN);
	SOCKADDR_IN clientSock;
	WCHAR clientIP[16];

	while (1)
	{
		pSession = new CSession;
		pSession->_socket = accept(listen_sock, (SOCKADDR *)&clientSock, &addrlen);

		if (pSession->_socket == INVALID_SOCKET)
		{
			Disconnect(pSession);
			continue;
		}

		InterlockedIncrement((LONG *)&_AcceptCounter);
		InetNtop(AF_INET, &clientSock.sin_addr, clientIP, 16);

		if (!OnConnectionRequest(clientIP, ntohs(clientSock.sin_port)))		// accept 직후
		{
			continue;
		}	//클라이언트 거부

		//Session 초기화
		wcscpy_s(pSession->_IP, 16, clientIP);
		pSession->_iPort = ntohs(clientSock.sin_port);
		pSession->RecvQ.ClearBuffer();
		pSession->SendQ.ClearBuffer();

		memset(&pSession->_RecvOverlapped, 0, sizeof(OVERLAPPED));
		memset(&pSession->_SendOverlapped, 0, sizeof(OVERLAPPED));

		pSession->_bSendFlag = FALSE;
		pSession->_lIOCount = 0;

		retval = CreateIoCompletionPort((HANDLE)pSession->_socket, hIOCP, (ULONG_PTR)pSession, 0);
		if (!retval)
			continue;

		//InterlockedIncrement64((LONG64 *)&pSession->_lIOCount);
		OnClientJoin(pSession, pSession->_iSessionID);

		RecvPost(pSession);
		InterlockedIncrement((LONG *)&iSessionCount);
	}

	return 0;
}

int CLanServer::MonitorThread_Update(LPVOID monitorArg)
{
	while (1)
	{
		_AcceptTPS = _AcceptCounter;
		_RecvPacketTPS = _RecvPacketCounter;
		_SendPacketTPS = _SendPacketCounter;

		_AcceptCounter = 0;
		_RecvPacketCounter = 0;
		_SendPacketCounter = 0;
		
		wprintf(L"------------------------------------------------\n");
		wprintf(L"Connect Session : %d\n", iSessionCount);
		wprintf(L"Accept TPS : %d\n", _AcceptTPS);
		wprintf(L"RecvPacket TPS : %d\n", _RecvPacketTPS);
		wprintf(L"SendPacket TPS : %d\n", _SendPacketTPS);
		wprintf(L"PacketPool Use : %d\n", 0);
		wprintf(L"PacketPool Alloc : %d\n", CNPacket::m_lRefCnt);
		wprintf(L"------------------------------------------------\n\n");

		Sleep(999);
	}
}

unsigned __stdcall CLanServer::WorkerThread(LPVOID workerArg)
{
	return ((CLanServer *)workerArg)->WorkerThread_Update(workerArg);
}

unsigned __stdcall CLanServer::AcceptThread(LPVOID acceptArg)
{
	return ((CLanServer *)acceptArg)->AcceptThread_Update(acceptArg);
}

unsigned __stdcall CLanServer::MonitorThread(LPVOID monitorArg)
{
	return ((CLanServer *)monitorArg)->MonitorThread_Update(monitorArg);
}

void CLanServer::RecvPost(CSession *pSession)
{
	int retval;
	DWORD dwRecvSize, dwflag = 0;
	WSABUF wBuf;

	wBuf.buf = pSession->RecvQ.GetWriteBufferPtr();
	wBuf.len = pSession->RecvQ.GetNotBrokenPutSize();

	InterlockedIncrement((LONG *)&pSession->_lIOCount);
	retval = WSARecv(pSession->_socket, &wBuf, 1, &dwRecvSize, &dwflag, &pSession->_RecvOverlapped, NULL);

	if (retval == SOCKET_ERROR)
	{
		int iErrorCode = GetLastError();
		if (iErrorCode != WSA_IO_PENDING)
		{
			OnError(iErrorCode, L"RecvPost Error\n");
			if (0 == InterlockedDecrement((LONG *)&pSession->_lIOCount))
				//Session Release
				ReleaseSession(pSession);

			return;
		}
	}
}

//----------------------------------------------------------------------------------------------------
// Send 등록 작업
//----------------------------------------------------------------------------------------------------
BOOL CLanServer::SendPost(CSession *pSession)
{
	int retval, iCount = 0;
	DWORD dwSendSize, dwflag = 0;
	WSABUF wBuf;

	wBuf.buf = pSession->SendQ.GetReadBufferPtr();
	wBuf.len = pSession->SendQ.GetUseSize();

	if (pSession->_bSendFlag == TRUE)	return FALSE;

	else{
		InterlockedIncrement((LONG *)&pSession->_lIOCount);
		pSession->_bSendFlag = TRUE;
		retval = WSASend(pSession->_socket, &wBuf, 1, &dwSendSize, dwflag, &pSession->_SendOverlapped, NULL);
		if (retval == SOCKET_ERROR)
		{
			int iErrorCode = GetLastError();
			if (iErrorCode != WSA_IO_PENDING)
			{
				OnError(iErrorCode, L"SendPost Error\n");
				if (0 == InterlockedDecrement(&pSession->_lIOCount))
					//Session Release
					ReleaseSession(pSession);

				return FALSE;
			}
		}
	}
	
	return TRUE;
}

bool CLanServer::PacketProc(CSession *pSession, CNPacket *pPacket)
{
	short header;

	//////////////////////////////////////////////////////////////////////////////////////////
	// RecvQ 용량이 header보다 작은지 검사
	//////////////////////////////////////////////////////////////////////////////////////////
	if (pSession->RecvQ.GetUseSize() < sizeof(header))
		return FALSE;

	pSession->RecvQ.Peek((char *)&header, sizeof(header));

	//////////////////////////////////////////////////////////////////////////////////////////
	//header + payload 용량이 RecvQ용량보다 큰지 검사
	//////////////////////////////////////////////////////////////////////////////////////////
	if (pSession->RecvQ.GetUseSize() < header + sizeof(header))
		return FALSE;

	*pPacket << header;
	pSession->RecvQ.RemoveData(header);

	//////////////////////////////////////////////////////////////////////////////////////////
	//payload를 cPacket에 뽑고 같은지 검사
	//////////////////////////////////////////////////////////////////////////////////////////
	if (header !=
		pSession->RecvQ.Get((char *)pPacket->GetBufferPtr(), header))
		return FALSE;

	return TRUE;
}

int CLanServer::GetClientCount()
{
	return iSessionCount;
}

void CLanServer::Disconnect(CSession *pSession)
{
	shutdown(pSession->_socket, SD_BOTH);
	pSession->_socket = INVALID_SOCKET;
}

void CLanServer::ReleaseSession(CSession *pSession)
{
	if (pSession->SendQ.GetUseSize() != 0)
		return;

	if (pSession->RecvQ.GetUseSize() != 0)
		return;

	shutdown(pSession->_socket, SD_BOTH);
	pSession->_socket = INVALID_SOCKET;
}
