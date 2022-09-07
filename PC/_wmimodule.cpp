//
// Helper library for querying WMI using its COM-based query API.
//
// Copyright (c) Microsoft Corporation
// Licensed to PSF under a contributor agreement
//

// Version history
//  2022-08: Initial contribution (Steve Dower)

#define _WIN32_DCOM
#include <Windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <propvarutil.h>

#include <Python.h>
#include "clinic/_wmimodule.cpp.h"


/*[clinic input]
module _wmi
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=7ca95dad1453d10d]*/



struct _query_data {
    LPCWSTR query;
    HANDLE writePipe;
    HANDLE readPipe;
};


static DWORD WINAPI
_query_thread(LPVOID param)
{
    IWbemLocator *locator = NULL;
    IWbemServices *services = NULL;
    IEnumWbemClassObject* enumerator = NULL;
    BSTR bstrQuery = NULL;
    struct _query_data *data = (struct _query_data*)param;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        CloseHandle(data->writePipe);
        return (DWORD)hr;
    }

    hr = CoInitializeSecurity(
        NULL, -1, NULL, NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE, NULL
    );
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(
            CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
            IID_IWbemLocator, (LPVOID *)&locator
        );
    }
    if (SUCCEEDED(hr)) {
        hr = locator->ConnectServer(
            bstr_t(L"ROOT\\CIMV2"),
            NULL, NULL, 0, NULL, 0, 0, &services
        );
    }
    if (SUCCEEDED(hr)) {
        hr = CoSetProxyBlanket(
            services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
            NULL, EOAC_NONE
        );
    }
    if (SUCCEEDED(hr)) {
        bstrQuery = SysAllocString(data->query);
        if (!bstrQuery) {
            hr = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = services->ExecQuery(
            bstr_t("WQL"), 
            bstrQuery,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, 
            NULL,
            &enumerator
        );
    }

    // Okay, after all that, at this stage we should have an enumerator
    // to the query results and can start writing them to the pipe!
    IWbemClassObject *value = NULL;
    int startOfEnum = TRUE;
    int endOfEnum = FALSE;
    while (SUCCEEDED(hr) && !endOfEnum) {
        ULONG got = 0;
        DWORD written;
        hr = enumerator->Next(WBEM_INFINITE, 1, &value, &got);
        if (hr == WBEM_S_FALSE) {
            // Could be at the end, but still got a result this time
            endOfEnum = TRUE;
            hr = 0;
            break;
        }
        if (FAILED(hr) || got != 1 || !value) {
            continue;
        }
        if (!startOfEnum && !WriteFile(data->writePipe, (LPVOID)L"\0", 2, &written, NULL)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        startOfEnum = FALSE;
        // Okay, now we have each resulting object it's time to
        // enumerate its members
        hr = value->BeginEnumeration(0);
        if (FAILED(hr)) {
            value->Release();
            break;
        }
        while (SUCCEEDED(hr)) {
            BSTR propName;
            VARIANT propValue;
            long flavor;
            hr = value->Next(0, &propName, &propValue, NULL, &flavor);
            if (hr == WBEM_S_NO_MORE_DATA) {
                hr = 0;
                break;
            }
            if (SUCCEEDED(hr) && (flavor & WBEM_FLAVOR_MASK_ORIGIN) != WBEM_FLAVOR_ORIGIN_SYSTEM) {
                WCHAR propStr[8192];
                hr = VariantToString(propValue, propStr, sizeof(propStr) / sizeof(propStr[0]));
                if (SUCCEEDED(hr)) {
                    DWORD cbStr1, cbStr2;
                    cbStr1 = (DWORD)(wcslen(propName) * sizeof(propName[0]));
                    cbStr2 = (DWORD)(wcslen(propStr) * sizeof(propStr[0]));
                    if (!WriteFile(data->writePipe, propName, cbStr1, &written, NULL) ||
                        !WriteFile(data->writePipe, (LPVOID)L"=", 2, &written, NULL) ||
                        !WriteFile(data->writePipe, propStr, cbStr2, &written, NULL) ||
                        !WriteFile(data->writePipe, (LPVOID)L"\0", 2, &written, NULL)
                    ) {
                        hr = HRESULT_FROM_WIN32(GetLastError());
                    }
                }
                VariantClear(&propValue);
                SysFreeString(propName);
            }
        }
        value->EndEnumeration();
        value->Release();
    }

    if (bstrQuery) {
        SysFreeString(bstrQuery);
    }
    if (enumerator) {
        enumerator->Release();
    }
    if (services) {
        services->Release();
    }
    if (locator) {
        locator->Release();
    }
    CoUninitialize();
    CloseHandle(data->writePipe);
    return (DWORD)hr;
}


/*[clinic input]
_wmi.exec_query

    query: unicode

Runs a WMI query against the local machine.

This returns a single string with 'name=value' pairs in a flat array separated
by null characters.
[clinic start generated code]*/

static PyObject *
_wmi_exec_query_impl(PyObject *module, PyObject *query)
/*[clinic end generated code: output=a62303d5bb5e003f input=48d2d0a1e1a7e3c2]*/

/*[clinic end generated code]*/
{
    PyObject *result = NULL;
    HANDLE hThread = NULL;
    int err = 0;
    WCHAR buffer[8192];
    DWORD offset = 0;
    DWORD bytesRead;
    struct _query_data data = {0};

    if (PySys_Audit("_wmi.exec_query", "O", query) < 0) {
        return NULL;
    }

    data.query = PyUnicode_AsWideCharString(query, NULL);
    if (!data.query) {
        return NULL;
    }

    if (0 != _wcsnicmp(data.query, L"select ", 7)) {
        PyMem_Free((void *)data.query);
        PyErr_SetString(PyExc_ValueError, "only SELECT queries are supported");
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS

    if (!CreatePipe(&data.readPipe, &data.writePipe, NULL, 0)) {
        err = GetLastError();
    } else {
        hThread = CreateThread(NULL, 0, _query_thread, (LPVOID*)&data, 0, NULL);
        if (!hThread) {
            err = GetLastError();
            // Normally the thread proc closes this handle, but since we never started
            // we need to close it here.
            CloseHandle(data.writePipe);
        }
    }

    while (!err) {
        if (ReadFile(
            data.readPipe,
            (LPVOID)&buffer[offset / sizeof(buffer[0])],
            sizeof(buffer) - offset,
            &bytesRead,
            NULL
        )) {
            offset += bytesRead;
            if (offset >= sizeof(buffer)) {
                err = ERROR_MORE_DATA;
            }
        } else {
            err = GetLastError();
        }
    }

    if (data.readPipe) {
        CloseHandle(data.readPipe);
    }

    // Allow the thread some time to clean up
    switch (WaitForSingleObject(hThread, 1000)) {
    case WAIT_OBJECT_0:
        // Thread ended cleanly
        if (!GetExitCodeThread(hThread, (LPDWORD)&err)) {
            err = GetLastError();
        }
        break;
    case WAIT_TIMEOUT:
        // Probably stuck - there's not much we can do, unfortunately
        if (err == 0 || err == ERROR_BROKEN_PIPE) {
            err = WAIT_TIMEOUT;
        }
        break;
    default:
        if (err == 0 || err == ERROR_BROKEN_PIPE) {
            err = GetLastError();
        }
        break;
    }

    CloseHandle(hThread);
    hThread = NULL;

    Py_END_ALLOW_THREADS

    PyMem_Free((void *)data.query);

    if (err == ERROR_MORE_DATA) {
        PyErr_Format(PyExc_OSError, "Query returns more than %zd characters", Py_ARRAY_LENGTH(buffer));
        return NULL;
    } else if (err) {
        PyErr_SetFromWindowsErr(err);
        return NULL;
    }

    if (!offset) {
        return PyUnicode_FromStringAndSize(NULL, 0);
    }
    return PyUnicode_FromWideChar(buffer, offset  / sizeof(buffer[0]) - 1);
}


static PyMethodDef wmi_functions[] = {
    _WMI_EXEC_QUERY_METHODDEF
    { NULL, NULL, 0, NULL }
};

static PyModuleDef wmi_def = {
    PyModuleDef_HEAD_INIT,
    "_wmi",
    NULL,   // doc
    0,      // m_size
    wmi_functions
};

extern "C" {
    PyMODINIT_FUNC PyInit__wmi(void)
    {
        return PyModuleDef_Init(&wmi_def);
    }
}
