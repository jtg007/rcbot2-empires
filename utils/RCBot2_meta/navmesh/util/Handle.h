/*
 * Handle.h
 *
 *  Created on: Apr 15, 2017
 */

#ifndef UTILS_VALVE_NAVMESH_HANDLE_H_
#define UTILS_VALVE_NAVMESH_HANDLE_H_

#include <basehandle.h>

// -------------------------------------------------------------------------------------------------- //
// CHandle.
// -------------------------------------------------------------------------------------------------- //
template<class T>
class CHandle: public CBaseHandle {
public:

	CHandle() {

	}

	CHandle(int iEntry, int iSerialNumber) {
		Init(iEntry, iSerialNumber);

	}

	CHandle(const CBaseHandle &handle) :
			CBaseHandle(handle) {

	}
	CHandle(T *pVal) {
		Term();
		Set(pVal);
	}

	// The index should have come from a call to ToInt(). If it hasn't, you're in trouble.
	static CHandle<T> FromIndex(int index) {
		CHandle<T> ret;
		ret.m_Index = index;
		return ret;
	}

	T* Get() const {
		return (T*)CBaseHandle::Get();
	}

	void Set(const T* pVal) {
		// The plugin build intentionally disables RTTI. The refactored nav
		// entity types are not IHandleEntity implementations, so keep these
		// optional handles invalid instead of relying on a cross-ABI cast.
		(void)pVal;
		CBaseHandle::Term();
	}
	operator T*() {
		return Get();
	}

	operator T*() const {
		return Get();
	}

	bool operator !() const {
		return !Get();
	}

	bool operator==(T *val) const {
		return Get() == val;
	}

	bool operator!=(T *val) const {
		return Get() != val;
	}

	const CBaseHandle& operator=(const T *val) {
		Set (val);
		return *this;
	}

	T* operator->() const {
		return Get();
	}
};




#endif /* UTILS_VALVE_NAVMESH_HANDLE_H_ */
