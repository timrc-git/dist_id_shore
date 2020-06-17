// Copyright 2020, Tim Crowder, All rights reserved.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <string>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef sockaddr       SOCKADDR;
typedef sockaddr_in    SOCKADDR_IN;
typedef struct ip_mreq MREQ; 
typedef int            SOCKET;
typedef hostent        HOSTENT;
typedef in_addr        IN_ADDR; 
typedef in_addr_t      IN_ADDR_T;
typedef socklen_t      SOCKLEN_T;
typedef void*          SOCKBUF_P;
typedef const void*    SOCKOPT_P;

// WARNING: there is a nasty bug in inet_ntop that uses one extra byte
//#define ADDR_STRLEN     (INET6_ADDRSTRLEN+1)
#define ADDR_STRLEN      (INET_ADDRSTRLEN+1)  
#define INVALID_SOCKET   -1
#define SOCKET_ERROR     -1
#define closesocket      close


class IPAddress {
public:
  SOCKADDR_IN ip;

public:
  IPAddress(const char* addr=NULL, int port=-1) {
    memset(&ip, 0, sizeof(ip));
    SetPort(port);
    if (addr) { SetAddress(addr); }
  }
  virtual ~IPAddress() {
  }

  bool operator==(IPAddress& other) {
    return 
      (ip.sin_addr.s_addr == other.ip.sin_addr.s_addr) &&
      (ip.sin_port == other.ip.sin_port);
  }
  bool operator!=(IPAddress& other) {
    return ! (*this == other);
  }

  virtual uint16_t GetPort() const { 
    return htons(ip.sin_port);
  }
  virtual void SetPort(int port) { 
    ip.sin_port = ntohs(port);
  }
  virtual int SetPort(const char* portStr) {
    int tmpPort = strtol(portStr, NULL, 10);
    if (tmpPort >=0) { 
      SetPort(tmpPort);
    } else { fprintf(stderr, "IPAddress - Invalid port value '%d'\n", tmpPort); }
    return tmpPort;
  }
  // sets the address and port from "a.b.c.d:port" form
  virtual int SetAddress(const char* addr) {
    if (!addr) { return -1; }
    // parse out {"a.b.c.d"|"host.domain"} ":" "port"
    if (!(addr && addr[0])) { return -1; }
    char* colonp = (char*)strchr(addr, ':');
    std::string addrOnlyStr(addr);
    if (colonp) {
      addrOnlyStr.assign(addr, colonp-addr);
      if (SetPort(colonp + 1) < 0) {
        fprintf(stderr, "IPAddress - Invalid port string '%s'\n", colonp); 
        return -1;
      }
    }

    const char* addrOnly = addrOnlyStr.c_str();
    if (addrOnlyStr == "*") {
      ip.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
      IN_ADDR_T at = inet_addr(addrOnly);
      memcpy(&(ip.sin_addr), &at, sizeof(at)); 
    }
    //If the address is not dotted notation, then do a DNS lookup of it.
    if (ip.sin_addr.s_addr == INADDR_NONE) {
      HOSTENT* host = gethostbyname(addrOnly);
      if (host) {
        memcpy(&(ip.sin_addr), host->h_addr_list[0], host->h_length); 
      } else {
        switch (h_errno) {
          case HOST_NOT_FOUND: fprintf(stderr, "IPAddress - Host Not Found\n");             break;
          case NO_ADDRESS:     fprintf(stderr, "IPAddress - Not a valid address (addr)\n"); break;
          //case NO_DATA:      fprintf(stderr, "IPAddress - Not a valid address (data)\n"); break;
          case NO_RECOVERY:    fprintf(stderr, "IPAddress - Name Server error\n");          break;
          case TRY_AGAIN:      fprintf(stderr, "IPAddress - Temporary DNS error\n");        break;
          default:             fprintf(stderr, "IPAddress - Unknown error\n");              break;
        }
        return 1;
      }
    }
    ip.sin_family = AF_INET;
    return 0;
  }

  virtual void GetString(std::string& addr) { // dotted numeric address
    char buf[ADDR_STRLEN];
    inet_ntop(AF_INET, (void*)&ip.sin_addr, buf, ADDR_STRLEN-1);
    addr=buf; addr+=":"; addr+=std::to_string(htons(ip.sin_port));
  }
  virtual bool IsMulticast() { return IN_MULTICAST(ntohl(ip.sin_addr.s_addr)); }
};


// check with:
//   sudo netstat -lnp
class UDPSocket {
public:
    IPAddress address;
    SOCKET sock;
    bool open;
public:
    UDPSocket() {
      sock=INVALID_SOCKET;
      open=false;
    }
    virtual ~UDPSocket() { if (open) { Close(); } }

    virtual bool IsOpen() { return open; }

    virtual bool GetAddress(IPAddress &addrActual) {
      socklen_t length = sizeof(addrActual.ip);
      if (SOCKET_ERROR == getsockname(sock, (struct sockaddr *) &addrActual.ip, &length)) {
        fprintf(stderr, "ERROR Socket::getsockname() FAILED!\n");
        return false;
      }
      return true;
    }

    virtual int Open(const char* addr=NULL) {
      // address may have already been set, so addr can be NULL
      if (addr) { address.SetAddress(addr); }
      sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (sock==INVALID_SOCKET) return 1;
      int yes = 1;
      setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      int ret=bind(sock, (SOCKADDR*)&(address.ip), sizeof(address.ip));
      open=(ret!=SOCKET_ERROR);
      IPAddress addrActual;
      if (open && GetAddress(addrActual)) { 
        std::string addrStr;
        addrActual.GetString(addrStr);
      }
      return open ? 0:1;
    }

    virtual int Close() {
      if (!open) { return 0; }
      if(sock!=INVALID_SOCKET) { closesocket(sock); sock=INVALID_SOCKET; }
      open=false;
      return 0;
    }

    virtual bool Wait(int timeout=-1, bool read=true) {
      if (sock==INVALID_SOCKET) { return false; }
      int status;
      //                tv_sec,  tv_usec
      timeval howlong = {0,      timeout};
      // negative timeout is infinite (NULL value to select)
      timeval *tp = timeout>=0 ? &howlong : NULL;
      if (tp == NULL) {
        fprintf(stderr, "INFINITE WAIT!\n");
      }
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(sock, &fds);
      if (read) {
        status = select(sock+1, &fds, NULL, NULL, tp);
      } else {
        status = select(sock+1, NULL, &fds, NULL, tp);
      }
      if (status<0) {
        fprintf(stderr, "IPSocket - Wait (select) error status=%d errno=%d [%s]\n", status, errno, strerror(errno));
      }
      return (status!=SOCKET_ERROR) && status;
    }

    virtual int Write(const char* buff, int sz) {
      if (!open) { return 0; }
      int ret=send(sock, (SOCKBUF_P)buff, sz, 0);
      if (ret==0 || ret==SOCKET_ERROR) { ret=0; open=false; }
      return ret;
    }
    virtual int WriteTo(IPAddress& addr, const char* buff, int sz) {
      if (!open) { return 1; }
      //fprintf(stderr, "UDPSocket - Send [%*.*s] to [%s:%d]\n", sz, sz, buff, addr.GetDotted(), addr.port);
      int ret=sendto(sock, (SOCKBUF_P)buff, sz, 0, (SOCKADDR*)&(addr.ip), sizeof(addr.ip));
      if (ret==0 || ret==SOCKET_ERROR) { ret=0; open=false; }
      return ret;
    }
    virtual int Read(char* buff, int maxSz) {
      if (!open) { return 0; }
      int ret = recv(sock, (SOCKBUF_P)buff, maxSz, 0);
      if (ret==0 || ret==SOCKET_ERROR) { ret=0; open=false; }
      return ret;
    }
    // reads data and puts sender address info in addr
    virtual int Read(char* buff, int maxSz, IPAddress& addr) {
      if (!open) { return 0; }
      SOCKLEN_T addrLen = sizeof(addr.ip);
      int ret = recvfrom(sock, (SOCKBUF_P)buff, maxSz, 0, (SOCKADDR*)&(addr.ip), &addrLen);
      if (ret==0 || ret==SOCKET_ERROR) { ret=0; open=false; }
      return ret;
    }
    virtual int ReadPacket(std::string& buff, int maxSz, IPAddress& addr) {
      if (!open) { return 0; }
      char dummy[8];
      int ret=0;
      SOCKLEN_T foo;
      int flags=MSG_TRUNC|MSG_PEEK;
      ret = recvfrom(sock, (SOCKBUF_P)dummy, 2, flags, (SOCKADDR*)&(addr.ip), &foo);
      if (ret>0) {
        buff.resize(ret); 
        ret = recvfrom(sock, (SOCKBUF_P)buff.data(), ret, 0, (SOCKADDR*)&(addr.ip), &foo);
      }
      if (ret==0 || ret==SOCKET_ERROR) { ret=0; open=false; }
      return ret;
    }
};


class MulticastSocket : public UDPSocket {
public:
    int open_flags;
    bool in_mc_group;

public:
    MulticastSocket() {
      in_mc_group = false;
      open_flags = 0;
    }
    virtual ~MulticastSocket() {
    }

    // flags is one of O_RDONLY, O_WRONLY, or O_RDWR
    virtual int Open(const char* addr, int flags=O_RDWR) {
      int ret;
      int yes = 1;
      // address may have already been set, so addr can be NULL
      if (addr) { address.SetAddress(addr); }
      //sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
      sock = socket(AF_INET, SOCK_DGRAM, 0);
      if (sock==INVALID_SOCKET) {
        fprintf(stderr, "MulticastSocket - Failed to create Socket\n");
        return 1;
      }

      fcntl(sock, F_SETFL, O_NONBLOCK);
      setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      if (flags==O_WRONLY && !address.IsMulticast()) {
        // special case ... if it's not multicast, 
        // then the reader and writer can't both be on the given address
        IPAddress tmpAddr;
        tmpAddr.ip.sin_family = AF_INET;
        tmpAddr.ip.sin_addr.s_addr = INADDR_ANY;
        ret=bind(sock, (SOCKADDR*)&(tmpAddr.ip), sizeof(tmpAddr.ip));
      } else {
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &yes, sizeof(yes));
        ret=bind(sock, (SOCKADDR*)&(address.ip), sizeof(address.ip));
      }
      if (ret) {
        fprintf(stderr, "MulticastSocket - Failed to bind Socket\n");
        return -1;
      }
      open=(ret!=SOCKET_ERROR);
      if (open && address.IsMulticast()) {
        if (!JoinMulticast(address)) {
          Close();
          return -1;
        }
        in_mc_group = true;
      }
      return open ? 0:1;
    }

    virtual int Close() {
      if (in_mc_group) { LeaveMulticast(address); }
      return UDPSocket::Close();
    }

    virtual bool JoinMulticast(IPAddress &multi) {
      MREQ request;
      memset(&request, 0, sizeof(request));
      request.imr_multiaddr = multi.ip.sin_addr;
      // TODO accept an interface arg at some point
      request.imr_interface.s_addr = INADDR_ANY;
      if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) < 0) {
        fprintf(stderr, "Failed to join multicast\n");
        return false;
      }
      return true;
    }

    virtual bool LeaveMulticast(IPAddress &multi) {
      MREQ request;
      memset(&request, 0, sizeof(request));
      request.imr_multiaddr = multi.ip.sin_addr;
      request.imr_interface.s_addr = INADDR_ANY;
      if (setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &request, sizeof(request)) < 0) {
        fprintf(stderr, "Failed to leave multicast\n");
        return false;
      }
      return true;
    }

    virtual bool SetTTL(int t) {
      if (address.IsMulticast()) {
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (void*)&t, sizeof(t)) < 0) {
          fprintf(stderr, "MulticastSocket - Failed to set multicast ttl\n");
          return -1;
        }
      }
      return 0;
    }
    virtual int GetTTL() {
      unsigned char ttl = 0;
      socklen_t     len=sizeof (ttl);
      if (address.IsMulticast()) {
        if (getsockopt (sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, &len) < 0) {
          fprintf(stderr, "MulticastSocket - Failed to get multicast ttl\n");
          return -1;
        }
      }
      return ttl;
    }
    virtual int Write(const char* buff, int sz) {
      // Always write specifically to the MC address
      return WriteTo(address, buff, sz);
    }
    bool GetMulticastInterface(sockaddr_in *address, bool skip_loopback=true) {
#define IFREQ_BUF_SIZE 8096
      // usable interface needs multicast/broadcast, and to be up
#define WANT_ATTRS (IFF_UP | IFF_BROADCAST | IFF_MULTICAST)
      struct sockaddr_in *s_addr;
      char ifreq_buf[IFREQ_BUF_SIZE];
      struct ifconf ifc;
      struct ifreq  *p_ifr;
      int i, len, sock;
      uint16_t port = address->sin_port;

      memset(address, 0, sizeof(*address));
      address->sin_family = PF_INET;
      address->sin_port = port;
      memset((char*)&ifc, 0, sizeof(ifc));
      memset((char*)ifreq_buf, 0, IFREQ_BUF_SIZE);
      ifc.ifc_len = IFREQ_BUF_SIZE;
      ifc.ifc_buf = ifreq_buf;

      /* open random socket, to enumerate interfaces */
      if (((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) ||
          (ioctl(sock, SIOCGIFCONF, &ifc) < 0)) {
        close(sock);
        return 0;
      }

      for(i=len=0; i<ifc.ifc_len; i+=len) {
        p_ifr=(struct ifreq*)(ifc.ifc_buf+i);
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
        len = sizeof p_ifr->ifr_name + p_ifr->ifr_addr.sa_len;
#else
        len = sizeof(struct ifreq);
#endif
        if ((ioctl(sock, SIOCGIFFLAGS, p_ifr) < 0) 
            || (skip_loopback && (p_ifr->ifr_flags & IFF_LOOPBACK)) 
            || (WANT_ATTRS ^ (p_ifr->ifr_flags & WANT_ATTRS)) 
            || (ioctl(sock, SIOCGIFADDR, p_ifr) < 0) ) { 
          continue; 
        }

        s_addr = (struct sockaddr_in *)&p_ifr->ifr_addr;
#if HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
        address->sin_len = sizeof(*address);
#endif
        memcpy(&address->sin_addr, (uint8_t*)&s_addr->sin_addr, sizeof(address->sin_addr));
        close(sock);
        return true;
      }

      close(sock);
      return false;
    }
};

