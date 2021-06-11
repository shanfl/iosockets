#ifdef _WIN32
#define _WIN32_WINNT 0x601
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <windows.h>

//#define errno WSAGetLastError()
#define close closesocket
#endif

#include <functional>
#include <iostream>
#include <map>
#include <vector>

enum class SockState
{
    SS_CLOSED = 0,
    SS_CLOSING,
    SS_CONNECTING,
    SS_CONNECTED,
    SS_LISTENING
};

enum class SEventType
{
    ET_CLOSE = 0,
    ET_CONNECT,
    ET_ERROR,
    ET_WRITE_READY,
};

struct SEventArgs
{
    SEventArgs(SEventType t, int ec) : etype(t), code(ec)
    {
        buff = nullptr;
        size = 0;
    }

    SEventType etype;
    int code;
    char *buff;
    size_t size;
};

class SockHandler
{
public:
    using ListenFnType = std::function<void(SockHandler *, SEventArgs)>;

public:
    uint64_t mId = 0;
    SOCKET mSocket = 0;
    SockState mState = SockState::SS_CLOSED;
    std::vector<char> mInBuf;
    std::vector<char> mOutBuf;

public:
    std::map<SEventType, ListenFnType> mListener;

public:
    SockHandler()
    {
        mSocket = socket(AF_INET, SOCK_STREAM, 0);
        this->SetNoBlock();
    }

    void SetNoDelay(int opt)
    {
        opt = !!opt;
        setsockopt(mSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt));
    }

    void SetNoBlock()
    {
        u_long type = 1;
        ioctlsocket(mSocket, FIONBIO, &type);
    }

    int Connect(std::string ip, int port,
                std::function<void(SockHandler *, SEventArgs)> fnCallBack)
    {
        mListener[SEventType::ET_CONNECT] = fnCallBack;
        struct sockaddr_in addr;
        addr.sin_port = port;
        addr.sin_family = AF_INET;
        inet_pton(addr.sin_family, ip.c_str(), &addr.sin_addr);
        int ret = connect(mSocket, (const sockaddr *)&addr, sizeof(addr));
        this->mState = SockState::SS_CONNECTING;
        return ret;
    }

    int Close()
    {
        if (mState == SockState::SS_CLOSED)
            return 1;

        mState = SockState::SS_CLOSED;

        if (mSocket != INVALID_SOCKET)
        {
            close(mSocket);
            mSocket = INVALID_SOCKET;
        }

        mInBuf.clear();
        mOutBuf.clear();
    }

    void On(SEventType type, ListenFnType fn) { mListener[type] = fn; }

public:
    void _on_connect_error()
    {
        mState = SockState::SS_CLOSED;
        SEventArgs args(SEventType::ET_CONNECT, 1);
        auto it = mListener.find(SEventType::ET_CONNECT);
        if (it != mListener.end())
            it->second(this, args);
    }

    void _on_connected()
    {
        mState = SockState::SS_CONNECTED;
        SEventArgs args(SEventType::ET_CONNECT, 0);
        auto it = mListener.find(SEventType::ET_CONNECT);
        if (it != mListener.end())
            it->second(this, args);
    }

    int _try_write()
    {
        if (mOutBuf.size())
        {
            int size = send(mSocket, &mOutBuf[0], mOutBuf.size(), 0);
            if (size <= 0)
            {
                if (errno == EWOULDBLOCK)
                    return 0;
                else
                {
                    Close();
                }
            }

            if (size == mOutBuf.size())
                mOutBuf.clear();
            else
            {
                mOutBuf.erase(mOutBuf.begin(), mOutBuf.begin() + size);
            }
        }

        if (mOutBuf.size() == 0)
        {
            if (mState == SockState::SS_CLOSING)
            {
                Close();
                return 0;
            }
        }

        return 1;
    }
};

enum class SelectSetType
{
    SST_READ = 0,
    SST_WRITE,
    SST_EXCEP,
    SST_MAX,
};

class SelectSet
{
public:
    SOCKET maxfd;
    fd_set fds[(int)(SelectSetType::SST_MAX)];

public:
    void zero()
    {
        maxfd = 0;
        for (int i = 0; i < (int)SelectSetType::SST_MAX; i++)
        {
            FD_ZERO(&fds[i]);
        }
    }
    void add(SockHandler *handler, SelectSetType typeSelect)
    {
        if (handler->mSocket > maxfd)
            maxfd = handler->mSocket;
        FD_SET(handler->mSocket, &fds[(int)typeSelect]);
    }

    bool ishas(SockHandler *handler, SelectSetType typeSelect)
    {
        return FD_ISSET(handler->mSocket, &fds[(int)typeSelect]);
    }
};

class SelectLoop
{
public:
    void Init()
    {
#ifdef _WIN32
        WSADATA dat;
        int err = WSAStartup(MAKEWORD(2, 2), &dat);
        if (err != 0)
        {
            std::cout << "WSAStartup failed " << err << std::endl;
        }
#endif
    }

    void UnInit() {}

    void update()
    {

        // clear closed socket
        //
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000;
        mSelectSet.zero();
        for (auto *handler : mConnects)
        {
            switch (handler->mState)
            {
            case SockState::SS_CONNECTING:
            {
                mSelectSet.add(handler, SelectSetType::SST_WRITE);
                mSelectSet.add(handler, SelectSetType::SST_EXCEP);
            }

            break;
            case SockState::SS_CONNECTED:
            {
                
            }
            break;
            case SockState::SS_CLOSING:
            {
            }
            break;
            case SockState::SS_CLOSED:
            {
            }
            break;
            }
        }

        select(mSelectSet.maxfd + 1, &mSelectSet.fds[0], &mSelectSet.fds[1],
               &mSelectSet.fds[2], &tv);

        for (auto *handler : mConnects)
        {
            switch (handler->mState)
            {
            case SockState::SS_CONNECTING:
            {
                if (mSelectSet.ishas(handler, SelectSetType::SST_WRITE))
                {
                    handler->mState = SockState::SS_CONNECTED;
                    int optval = 0;
                    socklen_t optlen = sizeof(optval);
                    getsockopt(handler->mSocket, SOL_SOCKET, SO_ERROR, (char *)&optval,
                               &optlen);
                    if (optval != 0)
                    {
                        // connect failed
                        handler->_on_connect_error();
                    }
                    else
                    {
                        handler->_on_connected();
                    }
                }
                else if (mSelectSet.ishas(handler, SelectSetType::SST_EXCEP))
                {
                    handler->_on_connect_error();
                }
            }
            }
            // send data which is getted during from this loop
            handler->_try_write();
        }
    }

    int Connect(std::string ip, int port,
                std::function<void(SockHandler *sh, SEventArgs)> listener)
    {
        SockHandler *handler = new SockHandler();
        mConnects.push_back(handler);
        return handler->Connect(ip, port, listener);
    }

    std::vector<SockHandler *> mConnects;
    SelectSet mSelectSet;
};

int main()
{
    SelectLoop loop;
    loop.Init();
    loop.Connect("127.0.0.1", 8080, [](auto *hanlder, SEventArgs args) -> void
                 {
                     std::cout << "hander->state:" << (int)hanlder->mState
                               << ",args.code:" << args.code;
                     if (args.code > 0) // ¡¥Ω” ß∞‹
                     {
                     }
                 });

    while (1)
    {
        loop.update();
    }

    return 0;
}