/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file
/// \brief SocketLayer class implementation
///


#include "SocketLayer.h"
#include "RakAssert.h"
#include "RakNetTypes.h"
#include "RakPeer.h"
#include "GetTime.h"
#include "LinuxStrings.h"
#include "SocketDefines.h"
#if (defined(__GNUC__)  || defined(__GCCXML__)) && !defined(__WIN32__)
#include <netdb.h>
#endif

using namespace RakNet;

/*
#if defined(__native_client__)
using namespace pp;
#endif
*/

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL!=1
#include "CCRakNetUDT.h"
#else
#include "CCRakNetSlidingWindow.h"
#endif

//SocketLayerOverride *SocketLayer::slo=0;

#ifdef _WIN32
#else
#include <string.h> // memcpy
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>  // error numbers
#include <stdio.h> // RAKNET_DEBUG_PRINTF
#if !defined(ANDROID)
#include <ifaddrs.h>
#endif
#include <netinet/in.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#endif













#if   defined(_WIN32)
#include "WSAStartupSingleton.h"
#include "WindowsIncludes.h"

#else
#include <unistd.h>
#endif

#include "RakSleep.h"
#include <stdio.h>
#include "Itoa.h"

#ifdef _MSC_VER
#pragma warning( push )
#endif

namespace RakNet
{
	extern void ProcessNetworkPacket( const SystemAddress systemAddress, const char *data, const int length, RakPeer *rakPeer, RakNet::TimeUS timeRead );
	//extern void ProcessNetworkPacket( const SystemAddress systemAddress, const char *data, const int length, RakPeer *rakPeer, RakNetSocket* rakNetSocket, RakNet::TimeUS timeRead );
}

#ifdef _DEBUG
#include <stdio.h>
#endif

 

// http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html#ip4to6
// http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html#getaddrinfo

#if RAKNET_SUPPORT_IPV6==1
void PrepareAddrInfoHints(addrinfo *hints)
{
	memset(hints, 0, sizeof (addrinfo)); // make sure the struct is empty
	hints->ai_socktype = SOCK_DGRAM; // UDP sockets
	hints->ai_flags = AI_PASSIVE;     // fill in my IP for me
}
#endif
 
void SocketLayer::SetSocketOptions( __UDPSOCKET__ listenSocket, bool blockingSocket, bool setBroadcast)
{
#ifdef __native_client__
	(void) listenSocket;
#else
	int sock_opt = 1;

	// This doubles the max throughput rate
	sock_opt=1024*256;
	setsockopt__(listenSocket, SOL_SOCKET, SO_RCVBUF, ( char * ) & sock_opt, sizeof ( sock_opt ) );

	// Immediate hard close. Don't linger the socket, or recreating the socket quickly on Vista fails.
	// Fail with voice and xbox

	sock_opt=0;
	setsockopt__(listenSocket, SOL_SOCKET, SO_LINGER, ( char * ) & sock_opt, sizeof ( sock_opt ) );



	// This doesn't make much difference: 10% maybe
	// Not supported on console 2
	sock_opt=1024*16;
	setsockopt__(listenSocket, SOL_SOCKET, SO_SNDBUF, ( char * ) & sock_opt, sizeof ( sock_opt ) );


	if (blockingSocket==false)
	{
#ifdef _WIN32
		unsigned long nonblocking = 1;
		ioctlsocket__(listenSocket, FIONBIO, &nonblocking );



#else
		fcntl( listenSocket, F_SETFL, O_NONBLOCK );
#endif
	}
	if (setBroadcast)
	{
		// Note: Fails with VDP but not xbox
		// Set broadcast capable
		sock_opt=1;
		if ( setsockopt__(listenSocket, SOL_SOCKET, SO_BROADCAST, ( char * ) & sock_opt, sizeof( sock_opt ) ) == -1 )
		{
#if defined(_WIN32) && defined(_DEBUG)
#if  !defined(WINDOWS_PHONE_8)
			DWORD dwIOError = GetLastError();
			// On Vista, can get WSAEACCESS (10013)
			// See http://support.microsoft.com/kb/819124
			// http://blogs.msdn.com/wndp/archive/2007/03/19/winsock-so-exclusiveaddruse-on-vista.aspx
			// http://msdn.microsoft.com/en-us/library/ms740621(VS.85).aspx
			LPVOID messageBuffer;
			FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
				( LPTSTR ) & messageBuffer, 0, NULL );
			// something has gone wrong here...
			RAKNET_DEBUG_PRINTF( "setsockopt__(SO_BROADCAST) failed:Error code - %d\n%s", dwIOError, messageBuffer );
			//Free the buffer.
			LocalFree( messageBuffer );
#endif
#endif

		}

	}

#endif
}
 

RakNet::RakString SocketLayer::GetSubNetForSocketAndIp(__UDPSOCKET__ inSock, RakNet::RakString inIpString)
{
	RakNet::RakString netMaskString;
	RakNet::RakString ipString;





#if   defined(WINDOWS_STORE_RT)
	RakAssert("Not yet supported" && 0);
	return "";
#elif defined(_WIN32)
	INTERFACE_INFO InterfaceList[20];
	unsigned long nBytesReturned;
	if (WSAIoctl(inSock, SIO_GET_INTERFACE_LIST, 0, 0, &InterfaceList,
		sizeof(InterfaceList), &nBytesReturned, 0, 0) == SOCKET_ERROR) {
			return "";
	}

	int nNumInterfaces = nBytesReturned / sizeof(INTERFACE_INFO);

	for (int i = 0; i < nNumInterfaces; ++i)
	{
		sockaddr_in *pAddress;
		pAddress = (sockaddr_in *) & (InterfaceList[i].iiAddress);
		ipString=inet_ntoa(pAddress->sin_addr);

		if (inIpString==ipString)
		{
			pAddress = (sockaddr_in *) & (InterfaceList[i].iiNetmask);
			netMaskString=inet_ntoa(pAddress->sin_addr);
			return netMaskString;
		}
	}
	return "";
#else

	int fd,fd2;
	fd2 = socket__(AF_INET, SOCK_DGRAM, 0);

	if(fd2 < 0)
	{
		return "";
	}

	struct ifconf ifc;
	char          buf[1999];
	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;
	if(ioctl(fd2, SIOCGIFCONF, &ifc) < 0)
	{
		return "";
	}

	struct ifreq *ifr;
	ifr         = ifc.ifc_req;
	int intNum = ifc.ifc_len / sizeof(struct ifreq);
	for(int i = 0; i < intNum; i++)
	{
		ipString=inet_ntoa(((struct sockaddr_in *)&ifr[i].ifr_addr)->sin_addr);

		if (inIpString==ipString)
		{
			struct ifreq ifr2;
			fd = socket__(AF_INET, SOCK_DGRAM, 0);
			if(fd < 0)
			{
				return "";
			}
			ifr2.ifr_addr.sa_family = AF_INET;

			strncpy(ifr2.ifr_name, ifr[i].ifr_name, IFNAMSIZ-1);

			ioctl(fd, SIOCGIFNETMASK, &ifr2);

			close(fd);
			close(fd2);
			netMaskString=inet_ntoa(((struct sockaddr_in *)&ifr2.ifr_addr)->sin_addr);

			return netMaskString;
		}
	}

	close(fd2);
	return "";

#endif

}












































































#if   defined(WINDOWS_STORE_RT)
void GetMyIP_WinRT( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] )
{
	// Perhaps DatagramSocket.BindEndpointAsynch, use localHostName as an empty string, then query what it bound to?
	RakAssert("Not yet supported" && 0);
}
#else
void GetMyIP_Win32( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] )
{
	int idx=0;
	idx=0;
	char ac[ 80 ];
	if ( gethostname( ac, sizeof( ac ) ) == -1 )
	{
 #if defined(_WIN32) && !defined(WINDOWS_PHONE_8)
		DWORD dwIOError = GetLastError();
		LPVOID messageBuffer;
		FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
			( LPTSTR ) & messageBuffer, 0, NULL );
		// something has gone wrong here...
		RAKNET_DEBUG_PRINTF( "gethostname failed:Error code - %d\n%s", dwIOError, (char*)messageBuffer );
		//Free the buffer.
		LocalFree( messageBuffer );
		#endif
		return ;
	}


#if RAKNET_SUPPORT_IPV6==1
	struct addrinfo hints;
	struct addrinfo *servinfo=0, *aip;  // will point to the results
	PrepareAddrInfoHints(&hints);
	getaddrinfo(ac, "", &hints, &servinfo);

	for (idx=0, aip = servinfo; aip != NULL && idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS; aip = aip->ai_next, idx++)
	{
		if (aip->ai_family == AF_INET)
		{
			struct sockaddr_in *ipv4 = (struct sockaddr_in *)aip->ai_addr;
			memcpy(&addresses[idx].address.addr4,ipv4,sizeof(sockaddr_in));
		}
		else
		{
			struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)aip->ai_addr;
			memcpy(&addresses[idx].address.addr4,ipv6,sizeof(sockaddr_in6));
		}

	}

	freeaddrinfo(servinfo); // free the linked-list
#else
	struct hostent *phe = gethostbyname( ac );

	if ( phe == 0 )
	{
 #if defined(_WIN32) && !defined(WINDOWS_PHONE_8)
		DWORD dwIOError = GetLastError();
		LPVOID messageBuffer;
		FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
			( LPTSTR ) & messageBuffer, 0, NULL );
		// something has gone wrong here...
		RAKNET_DEBUG_PRINTF( "gethostbyname failed:Error code - %d\n%s", dwIOError, messageBuffer );

		//Free the buffer.
		LocalFree( messageBuffer );
	#endif
		return ;
	}
	for ( idx = 0; idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS; ++idx )
	{
		if (phe->h_addr_list[ idx ] == 0)
			break;

		memcpy(&addresses[idx].address.addr4.sin_addr,phe->h_addr_list[ idx ],sizeof(struct in_addr));

	}
#endif // else RAKNET_SUPPORT_IPV6==1

	while (idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS)
	{
		addresses[idx]=UNASSIGNED_SYSTEM_ADDRESS;
		idx++;
	}
}

#endif


void SocketLayer::GetMyIP( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] )
{






#if   defined(WINDOWS_STORE_RT)
	GetMyIP_WinRT(addresses);
#elif defined(_WIN32)
	GetMyIP_Win32(addresses);
#else
//	GetMyIP_Linux(addresses);
	GetMyIP_Win32(addresses);
#endif
}


/*
unsigned short SocketLayer::GetLocalPort(RakNetSocket *s)
{
	SystemAddress sa;
	GetSystemAddress(s,&sa);
	return sa.GetPort();
}
*/
unsigned short SocketLayer::GetLocalPort(__UDPSOCKET__ s)
{
	SystemAddress sa;
	GetSystemAddress(s,&sa);
	return sa.GetPort();
}
void SocketLayer::GetSystemAddress_Old ( __UDPSOCKET__ s, SystemAddress *systemAddressOut )
{
#if defined(__native_client__)
	*systemAddressOut = UNASSIGNED_SYSTEM_ADDRESS;
#else
	sockaddr_in sa;
	memset(&sa,0,sizeof(sockaddr_in));
	socklen_t len = sizeof(sa);
	if (getsockname__(s, (sockaddr*)&sa, &len)!=0)
	{
#if defined(_WIN32) && defined(_DEBUG) && !defined(WINDOWS_PHONE_8)
		DWORD dwIOError = GetLastError();
		LPVOID messageBuffer;
		FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
			( LPTSTR ) & messageBuffer, 0, NULL );
		// something has gone wrong here...
		RAKNET_DEBUG_PRINTF( "getsockname failed:Error code - %d\n%s", dwIOError, messageBuffer );

		//Free the buffer.
		LocalFree( messageBuffer );
#endif
		*systemAddressOut = UNASSIGNED_SYSTEM_ADDRESS;
		return;
	}

	systemAddressOut->SetPortNetworkOrder(sa.sin_port);
	systemAddressOut->address.addr4.sin_addr.s_addr=sa.sin_addr.s_addr;
#endif
}
/*
void SocketLayer::GetSystemAddress_Old ( RakNetSocket *s, SystemAddress *systemAddressOut )
{
	return GetSystemAddress_Old(s->s, systemAddressOut);
}
*/
void SocketLayer::GetSystemAddress ( __UDPSOCKET__ s, SystemAddress *systemAddressOut )
{
#if RAKNET_SUPPORT_IPV6!=1
	GetSystemAddress_Old(s, systemAddressOut);
#else
	socklen_t slen;
	sockaddr_storage ss;
	slen = sizeof(ss);

	if (getsockname__(s, (struct sockaddr *)&ss, &slen)!=0)
	{
#if defined(_WIN32) && defined(_DEBUG)
		DWORD dwIOError = GetLastError();
		LPVOID messageBuffer;
		FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
			( LPTSTR ) & messageBuffer, 0, NULL );
		// something has gone wrong here...
		RAKNET_DEBUG_PRINTF( "getsockname failed:Error code - %d\n%s", dwIOError, messageBuffer );

		//Free the buffer.
		LocalFree( messageBuffer );
#endif
		systemAddressOut->FromString(0);
		return;
	}

	if (ss.ss_family==AF_INET)
	{
		memcpy(&systemAddressOut->address.addr4,(sockaddr_in *)&ss,sizeof(sockaddr_in));
		systemAddressOut->debugPort=ntohs(systemAddressOut->address.addr4.sin_port);

		uint32_t zero = 0;		
		if (memcmp(&systemAddressOut->address.addr4.sin_addr.s_addr, &zero, sizeof(zero))==0)
			systemAddressOut->SetToLoopback(4);
		//	systemAddressOut->address.addr4.sin_port=ntohs(systemAddressOut->address.addr4.sin_port);
	}
	else
	{
		memcpy(&systemAddressOut->address.addr6,(sockaddr_in6 *)&ss,sizeof(sockaddr_in6));
		systemAddressOut->debugPort=ntohs(systemAddressOut->address.addr6.sin6_port);

		char zero[16];
		memset(zero,0,sizeof(zero));
		if (memcmp(&systemAddressOut->address.addr4.sin_addr.s_addr, &zero, sizeof(zero))==0)
			systemAddressOut->SetToLoopback(6);

		//	systemAddressOut->address.addr6.sin6_port=ntohs(systemAddressOut->address.addr6.sin6_port);
	}
#endif // #if RAKNET_SUPPORT_IPV6!=1
}
/*
void SocketLayer::GetSystemAddress ( RakNetSocket *s, SystemAddress *systemAddressOut )
{
	return GetSystemAddress(s->s, systemAddressOut);
}
*/

// void SocketLayer::SetSocketLayerOverride(SocketLayerOverride *_slo)
// {
// 	slo=_slo;
// }

bool SocketLayer::GetFirstBindableIP(char firstBindable[128], int ipProto)
{
	SystemAddress ipList[ MAXIMUM_NUMBER_OF_INTERNAL_IDS ];
	SocketLayer::GetMyIP( ipList );


	if (ipProto==AF_UNSPEC)

	{
		ipList[0].ToString(false,firstBindable);
		return true;
	}		

	// Find the first valid host address
	unsigned int l;
	for (l=0; l < MAXIMUM_NUMBER_OF_INTERNAL_IDS; l++)
	{
		if (ipList[l]==UNASSIGNED_SYSTEM_ADDRESS)
			break;
		if (ipList[l].GetIPVersion()==4 && ipProto==AF_INET)
			break;
		if (ipList[l].GetIPVersion()==6 && ipProto==AF_INET6)
			break;
	}

	if (ipList[l]==UNASSIGNED_SYSTEM_ADDRESS || l==MAXIMUM_NUMBER_OF_INTERNAL_IDS)
		return false;
// 	RAKNET_DEBUG_PRINTF("%i %i %i %i\n",
// 		((char*)(&ipList[l].address.addr4.sin_addr.s_addr))[0],
// 		((char*)(&ipList[l].address.addr4.sin_addr.s_addr))[1],
// 		((char*)(&ipList[l].address.addr4.sin_addr.s_addr))[2],
// 		((char*)(&ipList[l].address.addr4.sin_addr.s_addr))[3]
// 	);
	ipList[l].ToString(false,firstBindable);
	return true;

}


#ifdef _MSC_VER
#pragma warning( pop )
#endif
