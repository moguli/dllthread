#include "dllthread.hpp"


struct dllthread::InitStruct {
	std::function<void()> m_fn;
	HANDLE m_threadStarted, m_threadEnded;
	volatile bool m_threadCancelled;
	InitStruct(std::function<void()>&& fn) :
		m_fn(std::move(fn)),
		m_threadStarted(INVALID_HANDLE_VALUE),
		m_threadEnded(INVALID_HANDLE_VALUE),
		m_threadCancelled(false)
	{
	}
	~InitStruct() {
		CloseHandle(m_threadStarted);
		m_threadStarted = INVALID_HANDLE_VALUE;
		CloseHandle(m_threadEnded);
		m_threadEnded = INVALID_HANDLE_VALUE;
	}
};

dllthread::~dllthread() {
	if (joinable())
		join();
}

void dllthread::join() {
	if (!joinable())
		throw std::invalid_argument("This thread is not joinable");
	if (GetCurrentThreadId() == m_id)
		throw std::runtime_error("Can't join. Possible deadlock.");

	// this is the most complex part
	// first of all we need to check whether the thread was started.
	// I give thread up to 3 seconds to start, if thread is not started in this time,
	// then most probably we are in DllMain function, thus, we are in deadlock situation, so
	// we need to emulate successfull thread join via thread termination.
	HANDLE handles[2] = { m_threadStarted, m_threadEnded };
	auto ret = WaitForMultipleObjects(sizeof(handles) / sizeof(*handles), handles, FALSE, 3000);
	switch (ret) {
		// m_threadStarted
		case WAIT_OBJECT_0:
			// thread started thus, going to default routine
			// check m_thread handle first (thread might be interrupted by system before)
			// and if thread is running then wait for the m_handle
			if (WaitForSingleObject(m_thread, 0) == WAIT_TIMEOUT)
				WaitForSingleObject(m_threadEnded, INFINITE);
			break;

		// m_threadEnded
		case (WAIT_OBJECT_0 + 1) :
			// success, thread runned & finished
			break;

		// in case of timeout or error, just detach
		case WAIT_TIMEOUT:
		default:
			// error, but we pretend that it is success, because thread wasn't even started yet.
			// force thread termination and pretend we are ok.
			m_initstruct->m_threadCancelled = true;
			MemoryBarrier();	// let all CPU receive this value
			SuspendThread(m_thread);
			TerminateThread(m_thread, 0);
			break;
	}
	detach();
}

void dllthread::detach() {
	if (!joinable())
		throw std::runtime_error("This thread is null or has been detached already");
	CloseHandle(m_thread);
	CloseHandle(m_threadStarted);
	CloseHandle(m_threadEnded);
	reset(*this);
}

DWORD WINAPI dllthread::threadstart(LPVOID param) {
	auto init = reinterpret_cast<InitStruct*>(param);
	// here's a check for deadlock. See SetEvent(m_threadEnded) in dllthread::join()
	SetEvent(init->m_threadStarted);
	if (!init->m_threadCancelled)
		init->m_fn();
	// TODO: here's the bottleneck. If you're joining thread within DLL_PROCESS_DETACH, this code below may not be
	// fully finish atomically. We need to SetEvent and ExitThread atomically before DLL will be unloaded.
	// This is a bug. A bad workaround would be to Sleep() for some amount of time, to let the thread to finish
	// or allow memory leak for init structure with SuspendThread() and TerminateThread() in join implementation.
	SetEvent(init->m_threadEnded);
	delete init;
	return 0;
}

void dllthread::init(std::function<void()>&& fn) {
	auto init = new InitStruct(std::forward<std::function<void()>>(fn));
	m_initstruct = init;
	try {
		;
		if ((m_threadStarted = CreateEvent(nullptr, false, false, nullptr)) == nullptr ||
			(m_threadEnded = CreateEvent(nullptr, false, false, nullptr)) == nullptr)
		{
			m_threadStarted = INVALID_HANDLE_VALUE;
			m_threadEnded = INVALID_HANDLE_VALUE;
			throw std::system_error(0, std::system_category(), "Can't create event");
		}
		HANDLE hProcess = GetCurrentProcess();
		if (!DuplicateHandle(hProcess, m_threadStarted, hProcess, &init->m_threadStarted, 0, FALSE, DUPLICATE_SAME_ACCESS))
			throw std::system_error(0, std::system_category(), "DuplicateHandle failed");
		if (!DuplicateHandle(hProcess, m_threadEnded, hProcess, &init->m_threadEnded, 0, FALSE, DUPLICATE_SAME_ACCESS))
			throw std::system_error(0, std::system_category(), "DuplicateHandle failed");
		m_thread = CreateThread(nullptr, 0, threadstart, init, 0, &m_id);
		if (m_thread == nullptr) {
			m_thread = INVALID_HANDLE_VALUE;
			throw std::system_error(0, std::system_category(), "Can't start new thread");
		}
	}
	catch (...) {
		CloseHandle(m_thread);
		CloseHandle(m_threadStarted);
		CloseHandle(m_threadEnded);
		delete init;
		throw;
	}
}

void dllthread::reset(dllthread& obj) {
	obj.m_threadStarted = INVALID_HANDLE_VALUE;
	obj.m_threadEnded = INVALID_HANDLE_VALUE;
	obj.m_thread = INVALID_HANDLE_VALUE;
	obj.m_id = 0;
}
