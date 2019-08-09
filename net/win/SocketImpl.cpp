#ifndef __linux__
#include "EventHandler.h"
#include "Buffer.h"
#include "WinExpendFunc.h"
#include "Log.h"
#include "IOCP.h"
#include "EventActions.h"
#include "SocketImpl.h"

using namespace cppnet;

CSocketImpl::CSocketImpl(std::shared_ptr<CEventActions>& event_actions) : CSocketImplBase(event_actions), _post_event_num(0) {
	_read_event = base::MakeNewSharedPtr<CEventHandler>(_pool.get());
	_write_event = base::MakeNewSharedPtr<CEventHandler>(_pool.get());

    _read_event->_data = _pool->PoolNew<EventOverlapped>();
    _read_event->_buffer = base::MakeNewSharedPtr<base::CBuffer>(_pool.get(), _pool);

    _write_event->_data = _pool->PoolNew<EventOverlapped>();
    _write_event->_buffer = base::MakeNewSharedPtr<base::CBuffer>(_pool.get(), _pool);
}

CSocketImpl::~CSocketImpl() {
    // remove from iocp
	if (_read_event && _read_event->_data) {
		EventOverlapped* temp = (EventOverlapped*)_read_event->_data;
		_pool->PoolDelete<EventOverlapped>(temp);
		_read_event->_data = nullptr;
	}
	if (_write_event && _write_event->_data) {
		EventOverlapped* temp = (EventOverlapped*)_write_event->_data;
		_pool->PoolDelete<EventOverlapped>(temp);
		_write_event->_data = nullptr;
	}
}

void CSocketImpl::SyncRead() {
	if (!_read_event->_call_back) {
		base::LOG_ERROR("call back function is null");
		return;
	}

	if (_event_actions) {
		_read_event->_event_flag_set = 0;
		_read_event->_event_flag_set |= EVENT_READ;
		if (_event_actions->AddRecvEvent(_read_event)) {
			_post_event_num++;
		}
	}
}

void CSocketImpl::SyncWrite(char* src, int len) {
	if (!_write_event->_call_back) {
        base::LOG_WARN("call back function is null, src : %s, len : %d", src, len);
		return;
	}

	_write_event->_buffer->Write(src, len);

	if (!_write_event->_client_socket) {
		_write_event->_client_socket = _read_event->_client_socket;
	}

	if (_event_actions) {
        _write_event->_event_flag_set = 0;
		_write_event->_event_flag_set |= EVENT_WRITE;
		if (_event_actions->AddSendEvent(_write_event)) {
			_post_event_num++;
		}
	}
}

void CSocketImpl::SyncConnection(const std::string& ip, short port, char* buf, int buf_len) {
	if (!_read_event->_call_back) {
        base::LOG_WARN("call back function is null, ip : %s, port : %d", ip.c_str(), port);
		return;
	}

	if (ip.length() > 16 || ip.empty()) {
        base::LOG_ERROR("a wrong ip! ip : %s", ip.c_str());
		return;
	}

    // set address info
	strcpy(_ip, ip.c_str());
	_port = port;

	if (!_read_event->_client_socket){
		_read_event->_client_socket = memshared_from_this();
	}

	if (_event_actions) {
		_read_event->_event_flag_set = 0;
		_read_event->_event_flag_set |= EVENT_CONNECT;
		if (_event_actions->AddConnection(_read_event, ip, port, buf, buf_len)) {
			_post_event_num++;
		}
	}
}

void CSocketImpl::SyncDisconnection() {
	if (!_read_event->_call_back) {
        base::LOG_WARN("call back function is null");
		return;
	}

	if (!_read_event->_client_socket) {
		_read_event->_client_socket = memshared_from_this();
	}

	if (_event_actions) {
		_read_event->_event_flag_set = 0;
		_read_event->_event_flag_set |= EVENT_DISCONNECT;
		if (_event_actions->AddDisconnection(_read_event)) {
			_post_event_num++;
		}
	}
}

void CSocketImpl::SyncRead(unsigned int interval) {

    SyncRead();

	if (_event_actions) {
		_read_event->_event_flag_set |= EVENT_TIMER;
		_event_actions->AddTimerEvent(interval, _read_event);
		_post_event_num++;
	}
}

void CSocketImpl::SyncWrite(unsigned int interval, char* src, int len) {

    SyncWrite(src, len);

	if (_event_actions) {
		_write_event->_event_flag_set |= EVENT_TIMER;
		_event_actions->AddTimerEvent(interval, _write_event);
		_post_event_num++;
	}
}

void CSocketImpl::PostTask(std::function<void(void)>& func) {
	_event_actions->PostTask(func);
}

void CSocketImpl::SetReadCallBack(const std::function<void(base::CMemSharePtr<CEventHandler>&, int error)>& call_back) {
	_read_event->_call_back = call_back;
}

void CSocketImpl::SetWriteCallBack(const std::function<void(base::CMemSharePtr<CEventHandler>&, int error)>& call_back) {
	_write_event->_call_back = call_back;
}

bool cppnet::operator>(const CSocketBase& s1, const CSocketBase& s2) {
	return s1._sock > s2._sock;
}

bool cppnet::operator<(const CSocketBase& s1, const CSocketBase& s2) {
	return s1._sock < s2._sock;
}

bool cppnet::operator==(const CSocketBase& s1, const CSocketBase& s2) {
	return s1._sock == s2._sock;
}

bool cppnet::operator!=(const CSocketBase& s1, const CSocketBase& s2) {
	return s1._sock != s2._sock;
}


void CSocketImpl::_Recv(base::CMemSharePtr<CEventHandler>& event) {
	EventOverlapped* context = (EventOverlapped*)event->_data;

	_post_event_num--;
	int err = -1;
	if (event->_event_flag_set & EVENT_TIMER) {
		err = EVENT_ERROR_TIMEOUT | event->_event_flag_set;
        event->_event_flag_set &= ~EVENT_TIMER;

	//get a connection event
	} else if (event->_event_flag_set == EVENT_CONNECT) {
		err = EVENT_ERROR_NO | event->_event_flag_set;

	} else if (event->_event_flag_set & EVENT_DISCONNECT) {
		err = EVENT_ERROR_NO | event->_event_flag_set;

	//get 0 bytes means close
	} else if (!event->_off_set) {
		if (_post_event_num == 0) {
			err = EVENT_ERROR_CLOSED | event->_event_flag_set;
		}

	} else {
		err = EVENT_ERROR_NO | event->_event_flag_set;
		event->_buffer->Write(context->_wsa_buf.buf, event->_off_set);
	}
	if (event->_call_back && err > -1) {
		event->_call_back(event, err);
		event->_event_flag_set = 0;
	}
}

void CSocketImpl::_Send(base::CMemSharePtr<CEventHandler>& event) {
	EventOverlapped* context = (EventOverlapped*)event->_data;

	_post_event_num--;
	int err = -1;
	if (event->_event_flag_set & EVENT_TIMER) {
		err = EVENT_ERROR_TIMEOUT | event->_event_flag_set;
        event->_event_flag_set &= ~EVENT_TIMER;

	} else if (!event->_off_set) {
		if (_post_event_num == 0) {
			err = EVENT_ERROR_CLOSED | event->_event_flag_set;
		}

	} else {
		err = EVENT_ERROR_NO | event->_event_flag_set;
	}

	if (event->_call_back && err > -1) {
		event->_call_back(event, err);
		event->_event_flag_set = 0;
	}
}

#endif