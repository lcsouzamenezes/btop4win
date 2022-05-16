/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include <fstream>
#include <ranges>
#include <cmath>
#include <numeric>
#include <mutex>
#include <chrono>
#include <locale>
#include <codecvt>
#include <semaphore>

#define _WIN32_DCOM
#define _WIN32_WINNT 0x0600
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <windows.h>

#include <winreg.h>
#pragma comment( lib, "Advapi32.lib" )
#include <winternl.h>
#pragma comment( lib, "ntdll.lib" )
#include <Pdh.h>
#pragma comment( lib, "Pdh.lib" )
#include <atlstr.h>
#include <tlhelp32.h>
#include <Psapi.h>
#pragma comment( lib, "Psapi.lib")
#include <comdef.h>
#include <Wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")
#include <winioctl.h>
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#include <powerbase.h>
#pragma comment(lib, "PowrProf.lib")

#define LODWORD(_qw)    ((DWORD)(_qw))
#define HIDWORD(_qw)    ((DWORD)(((_qw) >> 32) & 0xffffffff))

#include <btop_shared.hpp>
#include <btop_config.hpp>
#include <btop_tools.hpp>

using std::ifstream, std::numeric_limits, std::streamsize, std::round, std::max, std::min;
using std::clamp, std::string_literals::operator""s, std::cmp_equal, std::cmp_less, std::cmp_greater;
namespace fs = std::filesystem;
namespace rng = std::ranges;
using namespace Tools;

//? --------------------------------------------------- FUNCTIONS -----------------------------------------------------

namespace Tools {
	class HandleWrapper {
	public:
		HANDLE wHandle;
		bool valid = false;
		HandleWrapper() : wHandle(nullptr) { ; }
		HandleWrapper(HANDLE nHandle) : wHandle(nHandle) { valid = (wHandle != INVALID_HANDLE_VALUE); }
		auto operator()() { return wHandle; }
		~HandleWrapper() { if (wHandle != nullptr) CloseHandle(wHandle); }
	};

	//! Set security mode for better chance of collecting process information
	//! Based on code from psutil
	//! See: https://github.com/giampaolo/psutil/blob/master/psutil/arch/windows/security.c
	void setWinDebug() {
		HandleWrapper hToken{};
		HANDLE thisProc = GetCurrentProcess();

		if (not OpenProcessToken(thisProc, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken.wHandle)) {
			if (GetLastError() == ERROR_NO_TOKEN) {
				if (not ImpersonateSelf(SecurityImpersonation))
					throw std::runtime_error("setWinDebug() -> ImpersonateSelf() failed with ID: " + to_string(GetLastError()));
				if (not OpenProcessToken(thisProc, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken.wHandle))
					throw std::runtime_error("setWinDebug() -> OpenProcessToken() failed with ID: " + to_string(GetLastError()));
			}
			else
				throw std::runtime_error("setWinDebug() -> OpenProcessToken() failed with ID: " + to_string(GetLastError()));
		}

		TOKEN_PRIVILEGES tpriv;
		TOKEN_PRIVILEGES old_tpriv;
		LUID luid;
		DWORD tprivSize = sizeof(TOKEN_PRIVILEGES);

		if (not LookupPrivilegeValue(0, SE_DEBUG_NAME, &luid))
			throw std::runtime_error("setWinDebug() -> LookupPrivilegeValue() failed with ID: " + to_string(GetLastError()));

		tpriv.PrivilegeCount = 1;
		tpriv.Privileges[0].Luid = luid;
		tpriv.Privileges[0].Attributes = 0;

		if (not AdjustTokenPrivileges(hToken(), FALSE, &tpriv, tprivSize, &old_tpriv, &tprivSize))
			throw std::runtime_error("setWinDebug() -> AdjustTokenPrivileges() [get] failed with ID: " + to_string(GetLastError()));

		old_tpriv.PrivilegeCount = 1;
		old_tpriv.Privileges[0].Luid = luid;
		old_tpriv.Privileges[0].Attributes |= (SE_PRIVILEGE_ENABLED);

		if (not AdjustTokenPrivileges(hToken(), FALSE, &old_tpriv, tprivSize, 0, 0))
			throw std::runtime_error("setWinDebug() -> AdjustTokenPrivileges() [set] failed with ID: " + to_string(GetLastError()));

		RevertToSelf();
	}

	string bstr2str(BSTR source) {
		if (source == nullptr) return "";
		using convert_type = std::codecvt_utf8<wchar_t>;
		std::wstring_convert<convert_type, wchar_t> converter;
		return converter.to_bytes(_bstr_t(source));
	}
}

namespace Shared {
	IWbemServices* WbemServices;

	void WMI_init() {
		if (auto hr = CoInitializeEx(0, COINIT_MULTITHREADED); FAILED(hr))
			throw std::runtime_error("Shared::WMI_init() -> CoInitializeEx() failed with code: " + to_string(hr));
		if (auto hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL); FAILED(hr))
			throw std::runtime_error("Shared::WMI_init() -> CoInitializeSecurity() failed with code: " + to_string(hr));
		IWbemLocator* WbemLocator;
		if (auto hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&WbemLocator); FAILED(hr))
			throw std::runtime_error("Shared::WMI_init() -> CoCreateInstance() failed with code: " + to_string(hr));
		if (auto hr = WbemLocator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, NULL, 0, NULL, NULL, &WbemServices); FAILED(hr))
			throw std::runtime_error("Shared::WMI_init() -> ConnectServer() failed with code: " + to_string(hr));
		WbemLocator->Release();
		if (auto hr = CoSetProxyBlanket(WbemServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHN_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE); FAILED(hr))
			throw std::runtime_error("Shared::WMI_init() -> CoSetProxyBlanket() failed with code: " + to_string(hr));
	}

	class WbemEnumerator {
	public:
		IEnumWbemClassObject* WbEnum = nullptr;
		WbemEnumerator() { ; }
		auto operator()() { return WbEnum; }
		~WbemEnumerator() { if (WbEnum != nullptr) WbEnum->Release(); }
	};

	class WMIObjectReleaser {
	public:
		IWbemClassObject& WbClObj;
		WMIObjectReleaser(IWbemClassObject* obj) : WbClObj(*obj) { ; }
		~WMIObjectReleaser() { if (&WbClObj != nullptr) WbClObj.Release(); }
	};

	class VariantWrap {
	public:
		VARIANT val;
		VariantWrap() { ; }
		auto operator()() { return &val; }
		~VariantWrap() { VariantClear(&val); }
	};

}

namespace Mem {
	uint64_t old_systime;

	int64_t get_totalMem();
}

namespace Cpu {
	vector<long long> core_old_totals;
	vector<long long> core_old_idles;
	vector<string> available_fields;
	vector<string> available_sensors = { "Auto" };
	cpu_info current_cpu;
	bool got_sensors = false, cpu_temp_only = false;
	string gpu_name;
	bool has_gpu = false;
	atomic<uint64_t> smiTimer = 0;
	string smi_path;
	std::mutex SMImutex;
	std::binary_semaphore smi_work(0);
	inline bool SMI_wait() { return smi_work.try_acquire_for(std::chrono::milliseconds(100)); }
	inline void SMI_trigger() { smi_work.release(); }

	//* Populate found_sensors map
	bool get_sensors();

	string get_cpuName();

	struct Sensor {
		fs::path path;
		string label;
		int64_t temp = 0;
		int64_t high = 0;
		int64_t crit = 0;
	};

	GpuRaw GpuRawStats{};

	unordered_flat_map<string, Sensor> found_sensors;
	string cpu_sensor;
	vector<string> core_sensors;
	unordered_flat_map<int, int> core_mapping;

	//! Code for load average based on psutils calculation
	//! see https://github.com/giampaolo/psutil/blob/master/psutil/arch/windows/wmi.c

	const double LAVG_1F = 0.9200444146293232478931553241;
	const double LAVG_5F = 0.9834714538216174894737477501;
	const double LAVG_15F = 0.9944598480048967508795473394;
	double load_avg_1m = 0.0;
	double load_avg_5m = 0.0;
	double load_avg_15m = 0.0;

	void CALLBACK LoadAvgCallback(PVOID hCounter, BOOLEAN timedOut) {
		PDH_FMT_COUNTERVALUE displayValue;
		double currentLoad;
		
		if (PdhGetFormattedCounterValue((PDH_HCOUNTER)hCounter, PDH_FMT_DOUBLE, 0, &displayValue) != ERROR_SUCCESS) {
			return;
		}
		currentLoad = displayValue.doubleValue;

		load_avg_1m = load_avg_1m * LAVG_1F + currentLoad * (1.0 - LAVG_1F);
		load_avg_5m = load_avg_5m * LAVG_5F + currentLoad * (1.0 - LAVG_5F);
		load_avg_15m = load_avg_15m * LAVG_15F + currentLoad * (1.0 - LAVG_15F);
	}

	void loadAVG_init() {
		HQUERY hQuery;
		if (PdhOpenQueryW(nullptr, 0, &hQuery) != ERROR_SUCCESS) {
			throw std::runtime_error("Cpu::loadAVG_init() -> PdhOpenQueryW failed");
		}

		HCOUNTER hCounter;
		if (PdhAddEnglishCounterW(hQuery, L"\\System\\Processor Queue Length", 0, &hCounter) != ERROR_SUCCESS) {
			throw std::runtime_error("Cpu::loadAVG_init() -> PdhAddEnglishCounterW failed");
		}
		
		HANDLE eventh = CreateEventW(NULL, FALSE, FALSE, L"LoadUpdateEvent");
		if (eventh == NULL) {
			throw std::runtime_error("Cpu::loadAVG_init() -> CreateEventW failed");
		}

		
		if (PdhCollectQueryDataEx(hQuery, 5, eventh) != ERROR_SUCCESS) {
			throw std::runtime_error("Cpu::loadAVG_init() -> PdhCollectQueryDataEx failed");
		}

		HANDLE waitHandle;
		if (RegisterWaitForSingleObject(&waitHandle, eventh, (WAITORTIMERCALLBACK)LoadAvgCallback, (PVOID)hCounter, INFINITE, WT_EXECUTEDEFAULT) == 0) {
			throw std::runtime_error("Cpu::loadAVG_init() -> RegisterWaitForSingleObject failed");
		}
	}

	bool NvSMI_init() {
		array<char, 1024> sysdir;
		
		if (not GetSystemDirectoryA(sysdir.data(), 1024))
			return false;

		smi_path = sysdir.data();
		if (smi_path.empty())
			return false;

		smi_path += "\\nvidia-smi.exe";
		if (not fs::exists(smi_path)) {
			Logger::debug("Nvidia SMI not found. Disabling GPU monitoring.");
			return false;
		}

		string name;
		if (not ExecCMD(smi_path + " --query-gpu=gpu_name --format=csv,noheader", name)) {
			Logger::error("Error running Nvidia SMI. Disabling GPU monitoring. Output from nvidia-smi:");
			Logger::error(name);
			return false;
		}

		name.pop_back();
		name = s_replace(name, "NVIDIA ", "");
		name = s_replace(name, "GeForce ", "");
		gpu_name = name;

		return true;
	}

	//* Background thread for Nvidia SMI
	void NvSMI_runner() {
		while (not Global::quitting and has_gpu) {
			if (not SMI_wait()) continue;
			if (smiTimer > 0) sleep_ms(Config::getI("update_ms") - (smiTimer / 750));
			auto timeStart = time_micros();
			GpuRaw stats{};
			static string output;
			output.clear();

			if (ExecCMD(smi_path + " --query-gpu=utilization.gpu,clocks.gr,temperature.gpu,memory.total,memory.used --format=csv,noheader,nounits", output)) {
				try {
					auto outVec = ssplit(output, ',');
					if (outVec.size() != 5)
						throw std::runtime_error("Invalid number of return values.");

					stats.usage = stoull(outVec.at(0));
					stats.clock_mhz = ltrim(outVec.at(1)) + " Mhz";
					stats.temp = stoull(outVec.at(2));
					stats.mem_total = stoull(outVec.at(3));
					stats.mem_used = stoull(outVec.at(4));

				}
				catch (const std::exception& e) {
					Logger::error("Error running Nvidia SMI. Malformatted output. Disabling GPU monitoring.");
					Logger::error("NvSMi_runner() -> "s + e.what());
					has_gpu = false;
				}
			}
			else {
				Logger::error("Error running Nvidia SMI. Disabling GPU monitoring. Output from nvidia-smi:");
				Logger::error(output);
				has_gpu = false;
			}

			if (has_gpu) {
				std::lock_guard lck(SMImutex);
				GpuRawStats = stats;
			}
			else {
				Global::resized = true;
			}
			
			smiTimer = time_micros() - timeStart;
		}
	}
}

namespace Proc {

	struct WMIEntry {
		uint32_t ParentProcessId = 0;
		_bstr_t Name;
		_bstr_t CommandLine;
		_bstr_t ExecutablePath;
		_bstr_t KernelModeTime;
		_bstr_t UserModeTime;
		_bstr_t CreationDate;
		uint32_t ThreadCount = 0;
		_bstr_t PrivateMemory;
		_bstr_t ReadTransferCount;
		_bstr_t WriteTransferCount;
	};

	struct WMISvcEntry {
		bool AcceptPause = false;
		bool AcceptStop = false;
		_bstr_t Name;
		_bstr_t Caption;
		_bstr_t Description;
		uint32_t ProcessID;
		_bstr_t ServiceType;
		_bstr_t StartMode;
		_bstr_t Owner;
		_bstr_t State;
	};

	const struct WMIProcQuerys{
		_bstr_t WQL = L"WQL";
		_bstr_t SELECT = L"SELECT * FROM Win32_Process";
		_bstr_t SELECTSvc = L"SELECT * FROM Win32_Service";
		_bstr_t ProcessID = L"ProcessID";
		_bstr_t Name = L"Name";
		_bstr_t CommandLine = L"CommandLine";
		_bstr_t ExecutablePath = L"ExecutablePath";
		_bstr_t KernelModeTime = L"KernelModeTime";
		_bstr_t UserModeTime = L"UserModeTime";
		_bstr_t CreationDate = L"CreationDate";
		_bstr_t ThreadCount = L"ThreadCount";
		_bstr_t PrivateMemory = L"PrivatePageCount";
		_bstr_t ReadTransferCount = L"ReadTransferCount";
		_bstr_t WriteTransferCount = L"WriteTransferCount";
		_bstr_t ParentProcessId = L"ParentProcessId";
	};

	const struct WMISvcQuerys {
		_bstr_t WQL = L"WQL";
		_bstr_t SELECT = L"SELECT * FROM Win32_Service";
		_bstr_t ProcessID = L"ProcessID";
		_bstr_t AcceptPause = L"AcceptPause";
		_bstr_t AcceptStop = L"AcceptStop";
		_bstr_t Name = L"Name";
		_bstr_t Caption = L"Caption";
		_bstr_t Description = L"Description";
		_bstr_t StartMode = L"StartMode";
		_bstr_t Owner = L"StartName";
		_bstr_t State = L"State";

	};

	std::binary_semaphore wmi_work(0);
	inline bool WMI_wait() { return wmi_work.try_acquire_for(std::chrono::milliseconds(100)); }
	inline void WMI_trigger() { wmi_work.release(); }
	atomic<bool> WMI_running = false;
	atomic<uint64_t> WMItimer = 0;
	vector<size_t> WMI_requests;
	robin_hood::unordered_flat_map<size_t, WMIEntry> WMIList;
	robin_hood::unordered_flat_map<string, WMISvcEntry> WMISvcList;
	std::mutex WMImutex;

	//? WMI thread, collects process/service information once every second to augment missing information from the standard WIN32 API methods
	void WMICollect() {
		WMIProcQuerys QProc{};
		WMISvcQuerys QSvc{};
		int counter = 0;
		while (not Global::quitting) {
			if (not WMI_wait() and not (Config::getB("proc_services") and counter++ >= 50)) continue;
			counter = 0;
			vector<size_t> requests;
			atomic_wait(Runner::active);
			atomic_lock lck(WMI_running);
			requests.swap(WMI_requests);
			auto timeStart = time_micros();

			//* Processes
			{
				Shared::WbemEnumerator WMI;
				robin_hood::unordered_flat_map<size_t, WMIEntry> newWMIList = WMIList;
				auto& Q = QProc;
				vector<size_t> found;

				if (auto hr = Shared::WbemServices->ExecQuery(Q.WQL, Q.SELECT, WBEM_RETURN_WHEN_COMPLETE, 0, &WMI.WbEnum); FAILED(hr) or WMI() == nullptr) {
					throw std::runtime_error("Proc::WMICollect() (Processes) [thread] -> WbemServices query failed with code: " + to_string(hr));
				}

				IWbemClassObject* result = NULL;
				ULONG retCount = 0;

				while (WMI.WbEnum->Next(WBEM_INFINITE, 1, &result, &retCount) == S_OK) {
					Shared::WMIObjectReleaser rls(result);
					if (retCount == 0) break;
					size_t pid = 0;
					bool new_entry = false;
					
					Shared::VariantWrap ProcessId{};
					if (result->Get(Q.ProcessID, 0, &ProcessId.val, 0, 0) != S_OK) continue;
					pid = ProcessId()->uintVal;
					
					found.push_back(pid);
					if (pid == 0 or (not requests.empty() and not v_contains(requests, pid))) continue;

					if (not newWMIList.contains(pid)) {
						newWMIList[pid] = {};
						new_entry = true;
					}
					auto& entry = newWMIList.at(pid);
					
					Shared::VariantWrap ReadTransferCount{};
					if (result->Get(Q.ReadTransferCount, 0, &ReadTransferCount.val, 0, 0) == S_OK)
						entry.ReadTransferCount = ReadTransferCount()->bstrVal;
					
					Shared::VariantWrap WriteTransferCount{};
					if (result->Get(Q.WriteTransferCount, 0, &WriteTransferCount.val, 0, 0) == S_OK)
						entry.WriteTransferCount = WriteTransferCount()->bstrVal;

					Shared::VariantWrap PrivateMemory{};
					if (result->Get(Q.PrivateMemory, 0, &PrivateMemory.val, 0, 0) == S_OK)
						entry.PrivateMemory = PrivateMemory()->bstrVal;
					
					Shared::VariantWrap KernelModeTime{};
					if (result->Get(Q.KernelModeTime, 0, &KernelModeTime.val, 0, 0) == S_OK)
						entry.KernelModeTime = KernelModeTime()->bstrVal;
					
					Shared::VariantWrap UserModeTime{};
					if (result->Get(Q.UserModeTime, 0, &UserModeTime.val, 0, 0) == S_OK)
						entry.UserModeTime = UserModeTime()->bstrVal;
					
					Shared::VariantWrap ThreadCount{};
					if (result->Get(Q.ThreadCount, 0, &ThreadCount.val, 0, 0) == S_OK)
						entry.ThreadCount = ThreadCount()->uintVal;
					
					
					if (new_entry) {
						Shared::VariantWrap ParentProcessId{};
						if (result->Get(Q.ParentProcessId, 0, &ParentProcessId.val, 0, 0) == S_OK)
							entry.ParentProcessId = ParentProcessId()->uintVal;
					
						Shared::VariantWrap Name{};
						if (result->Get(Q.Name, 0, &Name.val, 0, 0) == S_OK)
							entry.Name = Name()->bstrVal;
					
						Shared::VariantWrap CommandLine{};
						if (result->Get(Q.CommandLine, 0, &CommandLine.val, 0, 0) == S_OK)
							entry.CommandLine = CommandLine()->bstrVal;
					
						Shared::VariantWrap ExecutablePath{};
						if (result->Get(Q.ExecutablePath, 0, &ExecutablePath.val, 0, 0) == S_OK)
							entry.ExecutablePath = ExecutablePath()->bstrVal;

						Shared::VariantWrap CreationDate{};
						if (result->Get(Q.CreationDate, 0, &CreationDate.val, 0, 0) == S_OK)
							entry.CreationDate = CreationDate()->bstrVal;
					}


				}

				//? Clear dead processes from list
				for (auto it = newWMIList.begin(); it != newWMIList.end();) {
					if (not v_contains(found, it->first))
						it = newWMIList.erase(it);
					else
						it++;
				}

				const std::lock_guard<std::mutex> lck(Proc::WMImutex);
				Proc::WMIList.swap(newWMIList);
			}
				
			//* Services
			if (Config::getB("proc_services") or WMISvcList.empty()) {
				Shared::WbemEnumerator WMI;
				robin_hood::unordered_flat_map<string, WMISvcEntry> newWMISvcList = WMISvcList;
				auto& Q = QSvc;
				vector<string> found;

				if (auto hr = Shared::WbemServices->ExecQuery(Q.WQL, Q.SELECT, WBEM_RETURN_WHEN_COMPLETE, 0, &WMI.WbEnum); FAILED(hr) or WMI() == nullptr) {
					throw std::runtime_error("Proc::WMICollect() (Services) [thread] -> WbemServices query failed with code: " + to_string(hr));
				}

				IWbemClassObject* result = NULL;
				ULONG retCount = 0;

				while (WMI.WbEnum->Next(WBEM_INFINITE, 1, &result, &retCount) == S_OK) {
					Shared::WMIObjectReleaser rls(result);
					if (retCount == 0) break;
					string name;
					bool new_entry = false;
					
					Shared::VariantWrap Name{};
					if (result->Get(Q.Name, 0, &Name.val, 0, 0) == S_OK)
						name = bstr2str(Name()->bstrVal);
					
					if (name.empty()) continue;
					found.push_back(name);
					if (not newWMISvcList.contains(name)) {
						newWMISvcList[name] = {};
						new_entry = true;
					}
					auto& entry = newWMISvcList.at(name);
					
					Shared::VariantWrap ProcessId{};
					if (result->Get(Q.ProcessID, 0, &ProcessId.val, 0, 0) == S_OK)
						entry.ProcessID = ProcessId()->uintVal;
					
					Shared::VariantWrap StartMode{};
					if (result->Get(Q.StartMode, 0, &StartMode.val, 0, 0) == S_OK)
						entry.StartMode = StartMode()->bstrVal;
					
					Shared::VariantWrap AcceptPause{};
					if (result->Get(Q.AcceptPause, 0, &AcceptPause.val, 0, 0) == S_OK)
						entry.AcceptPause = (AcceptPause()->boolVal == VARIANT_TRUE);
					
					Shared::VariantWrap AcceptStop{};
					if (result->Get(Q.AcceptStop, 0, &AcceptStop.val, 0, 0) == S_OK)
						entry.AcceptStop = (AcceptStop()->boolVal == VARIANT_TRUE);
					
					Shared::VariantWrap Owner{};
					if (result->Get(Q.Owner, 0, &Owner.val, 0, 0) == S_OK)
						entry.Owner = Owner()->bstrVal;
					
					Shared::VariantWrap State{};
					if (result->Get(Q.State, 0, &State.val, 0, 0) == S_OK)
						entry.State = State()->bstrVal;
					
					if (new_entry) {
						Shared::VariantWrap Caption{};
						if (result->Get(Q.Caption, 0, &Caption.val, 0, 0) == S_OK)
							entry.Caption = Caption()->bstrVal;
					
						Shared::VariantWrap Description{};
						if (result->Get(Q.Description, 0, &Description.val, 0, 0) == S_OK)
							entry.Description = Description()->bstrVal;
					}
					

				}
				
				//? Clear missing services from list
				for (auto it = newWMISvcList.begin(); it != newWMISvcList.end();) {
					if (not v_contains(found, it->first))
						it = newWMISvcList.erase(it);
					else
						it++;
				}
				
				const std::lock_guard<std::mutex> lck(Proc::WMImutex);
				Proc::WMISvcList.swap(newWMISvcList);
			}

			//auto timeDone = time_micros();
			Proc::WMItimer = time_micros() - timeStart;

			/*auto timeNext = timeStart + 1'000'000;
			if (auto timeNow = time_micros(); timeNext > timeNow) 
				sleep_micros(timeNext - timeNow);*/
		}
	}
}

namespace Shared {

	fs::path procPath, passwd_path;
	long pageSize, clkTck, coreCount;

	void init() {

		//? Shared global variables init
		procPath = "";
		passwd_path = "";

		//? Set SE DEBUG mode
		try {
			setWinDebug();
		}
		catch (const std::exception& e) {
			Logger::warning("Failed to set SE DEBUG mode for process!");
			Logger::debug(e.what());
		}

		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);

		coreCount = sysinfo.dwNumberOfProcessors;
		if (coreCount < 1) {
			throw std::runtime_error("Could not determine number of cores!");
		}

		pageSize = sysinfo.dwPageSize;
		if (pageSize <= 0) {
			pageSize = 4096;
			Logger::warning("Could not get system page size. Defaulting to 4096, processes memory usage might be incorrect.");
		}

		clkTck = 100;
		if (clkTck <= 0) {
			clkTck = 100;
			Logger::warning("Could not get system clock ticks per second. Defaulting to 100, processes cpu usage might be incorrect.");
		}

		//? Init for namespace Cpu
		Cpu::current_cpu.core_percent.insert(Cpu::current_cpu.core_percent.begin(), Shared::coreCount, {});
		Cpu::current_cpu.temp.insert(Cpu::current_cpu.temp.begin(), Shared::coreCount + 1, {});
		Cpu::core_old_totals.insert(Cpu::core_old_totals.begin(), Shared::coreCount, 0);
		Cpu::core_old_idles.insert(Cpu::core_old_idles.begin(), Shared::coreCount, 0);
		Cpu::collect();
		for (auto& [field, vec] : Cpu::current_cpu.cpu_percent) {
			if (not vec.empty()) Cpu::available_fields.push_back(field);
		}
		Cpu::cpuName = Cpu::get_cpuName();
		Cpu::got_sensors = Cpu::get_sensors();
		for (const auto& [sensor, ignored] : Cpu::found_sensors) {
			Cpu::available_sensors.push_back(sensor);
		}
		Cpu::core_mapping = Cpu::get_core_mapping();

		//? Start up loadAVG counter in background
		std::thread(Cpu::loadAVG_init).detach();

		//? Init for namespace Mem
		Mem::old_systime = GetTickCount64();
		Mem::collect();

		//? Set up connection to WMI
		Shared::WMI_init();

		//? Start up WMI system info collector in background
		std::thread(Proc::WMICollect).detach();
		Proc::WMI_trigger();

		Cpu::has_gpu = Cpu::NvSMI_init();
		if (Cpu::has_gpu) {
			std::thread(Cpu::NvSMI_runner).detach();
			Cpu::available_fields.push_back("gpu");
		}
		

	}

}

namespace Cpu {
	string cpuName;
	string cpuHz;
	string gpu_clock;
	bool has_battery = true;
	tuple<int, long, string> current_bat;

	const array<string, 6> time_names = { "kernel", "user", "dpc", "interrupt", "idle" };

	unordered_flat_map<string, long long> cpu_old = {
			{"total", 0},
			{"kernel", 0},
			{"user", 0},
			{"dpc", 0},
			{"interrupt", 0},
			{"idle", 0},
			{"totals", 0},
			{"idles", 0}
		};

	typedef struct _PROCESSOR_POWER_INFORMATION {
		ULONG Number;
		ULONG MaxMhz;
		ULONG CurrentMhz;
		ULONG MhzLimit;
		ULONG MaxIdleState;
		ULONG CurrentIdleState;
	} PROCESSOR_POWER_INFORMATION, * PPROCESSOR_POWER_INFORMATION;

	string get_cpuHz() {
		static bool failed = false;
		if (failed) return "";
		uint64_t hz = 0;
		string cpuhz;

		vector<PROCESSOR_POWER_INFORMATION> ppinfo(Shared::coreCount);

		if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, &ppinfo[0], Shared::coreCount * sizeof(PROCESSOR_POWER_INFORMATION)) != 0) {
			Logger::warning("Cpu::get_cpuHz() -> CallNtPowerInformation() failed");
			failed = true;
			return "";
		}

		hz = ppinfo[0].CurrentMhz;

		if (hz <= 1 or hz >= 1000000) {
			Logger::warning("Cpu::get_cpuHz() -> Got invalid cpu mhz value");
			failed = true;
			return "";
		}

		if (hz >= 1000) {
			if (hz >= 10000) cpuhz = to_string((int)round(hz / 1000));
			else cpuhz = to_string(round(hz / 100) / 10.0).substr(0, 3);
			cpuhz += " GHz";
		}
		else if (hz > 0)
			cpuhz = to_string((int)round(hz)) + " MHz";

		return cpuhz;
	}

	string get_cpuName() {
		string name;
		HKEY hKey;

		if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
			wchar_t cpuName[255];
			DWORD BufSize = sizeof(cpuName);
			if (RegQueryValueEx(hKey, L"ProcessorNameString", NULL, NULL, (LPBYTE)cpuName, &BufSize) == ERROR_SUCCESS) {
				name = string(CW2A(cpuName));
			}
		}

		if (name.empty()) return "";

		auto name_vec = ssplit(name);

		if ((s_contains(name, "Xeon"s) or v_contains(name_vec, "Duo"s)) and v_contains(name_vec, "CPU"s)) {
			auto cpu_pos = v_index(name_vec, "CPU"s);
			if (cpu_pos < name_vec.size() - 1 and not name_vec.at(cpu_pos + 1).ends_with(')'))
				name = name_vec.at(cpu_pos + 1);
			else
				name.clear();
		}
		else if (v_contains(name_vec, "Ryzen"s)) {
			auto ryz_pos = v_index(name_vec, "Ryzen"s);
			name = "Ryzen"	+ (ryz_pos < name_vec.size() - 1 ? ' ' + name_vec.at(ryz_pos + 1) : "")
							+ (ryz_pos < name_vec.size() - 2 ? ' ' + name_vec.at(ryz_pos + 2) : "");
		}
		else if (s_contains(name, "Intel"s) and v_contains(name_vec, "CPU"s)) {
			auto cpu_pos = v_index(name_vec, "CPU"s);
			if (cpu_pos < name_vec.size() - 1 and not name_vec.at(cpu_pos + 1).ends_with(')') and name_vec.at(cpu_pos + 1) != "@")
				name = name_vec.at(cpu_pos + 1);
			else
				name.clear();
		}
		else
			name.clear();

		if (name.empty() and not name_vec.empty()) {
			for (const auto& n : name_vec) {
				if (n == "@") break;
				name += n + ' ';
			}
			name.pop_back();
			for (const auto& replace : {"Processor", "CPU", "(R)", "(TM)", "Intel", "AMD", "Core"}) {
				name = s_replace(name, replace, "");
				name = s_replace(name, "  ", " ");
			}
			name = trim(name);
		}
		

		return name;
	}

	bool get_sensors() {
		bool got_cpu = false, got_coretemp = false;
		vector<fs::path> search_paths;
		//try {
		//	//? Setup up paths to search for sensors
		//	if (fs::exists(fs::path("/sys/class/hwmon")) and access("/sys/class/hwmon", R_OK) != -1) {
		//		for (const auto& dir : fs::directory_iterator(fs::path("/sys/class/hwmon"))) {
		//			fs::path add_path = fs::canonical(dir.path());
		//			if (v_contains(search_paths, add_path) or v_contains(search_paths, add_path / "device")) continue;

		//			if (s_contains(add_path, "coretemp"))
		//				got_coretemp = true;

		//			for (const auto & file : fs::directory_iterator(add_path)) {
		//				if (string(file.path().filename()) == "device") {
		//					for (const auto & dev_file : fs::directory_iterator(file.path())) {
		//						string dev_filename = dev_file.path().filename();
		//						if (dev_filename.starts_with("temp") and dev_filename.ends_with("_input")) {
		//							search_paths.push_back(file.path());
		//							break;
		//						}
		//					}
		//				}

		//				string filename = file.path().filename();
		//				if (filename.starts_with("temp") and filename.ends_with("_input")) {
		//					search_paths.push_back(add_path);
		//					break;
		//				}
		//			}
		//		}
		//	}
		//	if (not got_coretemp and fs::exists(fs::path("/sys/devices/platform/coretemp.0/hwmon"))) {
		//		for (auto& d : fs::directory_iterator(fs::path("/sys/devices/platform/coretemp.0/hwmon"))) {
		//			fs::path add_path = fs::canonical(d.path());

		//			for (const auto & file : fs::directory_iterator(add_path)) {
		//				string filename = file.path().filename();
		//				if (filename.starts_with("temp") and filename.ends_with("_input") and not v_contains(search_paths, add_path)) {
		//						search_paths.push_back(add_path);
		//						got_coretemp = true;
		//						break;
		//				}
		//			}
		//		}
		//	}
		//	//? Scan any found directories for temperature sensors
		//	if (not search_paths.empty()) {
		//		for (const auto& path : search_paths) {
		//			const string pname = readfile(path / "name", path.filename());
		//			for (const auto & file : fs::directory_iterator(path)) {
		//				const string file_suffix = "input";
		//				const int file_id = atoi(file.path().filename().c_str() + 4); // skip "temp" prefix
		//				string file_path = file.path();

		//				if (!s_contains(file_path, file_suffix)) {
		//					continue;
		//				}

		//				const string basepath = file_path.erase(file_path.find(file_suffix), file_suffix.length());
		//				const string label = readfile(fs::path(basepath + "label"), "temp" + to_string(file_id));
		//				const string sensor_name = pname + "/" + label;
		//				const int64_t temp = stol(readfile(fs::path(basepath + "input"), "0")) / 1000;
		//				const int64_t high = stol(readfile(fs::path(basepath + "max"), "80000")) / 1000;
		//				const int64_t crit = stol(readfile(fs::path(basepath + "crit"), "95000")) / 1000;

		//				found_sensors[sensor_name] = {fs::path(basepath + "input"), label, temp, high, crit};

		//				if (not got_cpu and (label.starts_with("Package id") or label.starts_with("Tdie"))) {
		//					got_cpu = true;
		//					cpu_sensor = sensor_name;
		//				}
		//				else if (label.starts_with("Core") or label.starts_with("Tccd")) {
		//					got_coretemp = true;
		//					if (not v_contains(core_sensors, sensor_name)) core_sensors.push_back(sensor_name);
		//				}
		//			}
		//		}
		//	}
		//	//? If no good candidate for cpu temp has been found scan /sys/class/thermal
		//	if (not got_cpu and fs::exists(fs::path("/sys/class/thermal"))) {
		//		const string rootpath = fs::path("/sys/class/thermal/thermal_zone");
		//		for (int i = 0; fs::exists(fs::path(rootpath + to_string(i))); i++) {
		//			const fs::path basepath = rootpath + to_string(i);
		//			if (not fs::exists(basepath / "temp")) continue;
		//			const string label = readfile(basepath / "type", "temp" + to_string(i));
		//			const string sensor_name = "thermal" + to_string(i) + "/" + label;
		//			const int64_t temp = stol(readfile(basepath / "temp", "0")) / 1000;

		//			int64_t high, crit;
		//			for (int ii = 0; fs::exists(basepath / string("trip_point_" + to_string(ii) + "_temp")); ii++) {
		//				const string trip_type = readfile(basepath / string("trip_point_" + to_string(ii) + "_type"));
		//				if (not is_in(trip_type, "high", "critical")) continue;
		//				auto& val = (trip_type == "high" ? high : crit);
		//				val = stol(readfile(basepath / string("trip_point_" + to_string(ii) + "_temp"), "0")) / 1000;
		//			}
		//			if (high < 1) high = 80;
		//			if (crit < 1) crit = 95;

		//			found_sensors[sensor_name] = {basepath / "temp", label, temp, high, crit};
		//		}
		//	}

		//}
		//catch (...) {}

		//if (not got_coretemp or core_sensors.empty()) {
		//	cpu_temp_only = true;
		//}
		//else {
		//	rng::sort(core_sensors, rng::less{});
		//	rng::stable_sort(core_sensors, [](const auto& a, const auto& b){
		//		return a.size() < b.size();
		//	});
		//}

		//if (cpu_sensor.empty() and not found_sensors.empty()) {
		//	for (const auto& [name, sensor] : found_sensors) {
		//		if (s_contains(str_to_lower(name), "cpu") or s_contains(str_to_lower(name), "k10temp")) {
		//			cpu_sensor = name;
		//			break;
		//		}
		//	}
		//	if (cpu_sensor.empty()) {
		//		cpu_sensor = found_sensors.begin()->first;
		//		Logger::warning("No good candidate for cpu sensor found, using random from all found sensors.");
		//	}
		//}

		return not found_sensors.empty();
	}

	void update_sensors() {
		if (cpu_sensor.empty()) return;

		//const auto& cpu_sensor = (not Config::getS("cpu_sensor").empty() and found_sensors.contains(Config::getS("cpu_sensor")) ? Config::getS("cpu_sensor") : Cpu::cpu_sensor);

		//found_sensors.at(cpu_sensor).temp = stol(readfile(found_sensors.at(cpu_sensor).path, "0")) / 1000;
		//current_cpu.temp.at(0).push_back(found_sensors.at(cpu_sensor).temp);
		//current_cpu.temp_max = found_sensors.at(cpu_sensor).crit;
		//if (current_cpu.temp.at(0).size() > 20) current_cpu.temp.at(0).pop_front();

		//if (Config::getB("show_coretemp") and not cpu_temp_only) {
		//	vector<string> done;
		//	for (const auto& sensor : core_sensors) {
		//		if (v_contains(done, sensor)) continue;
		//		found_sensors.at(sensor).temp = stol(readfile(found_sensors.at(sensor).path, "0")) / 1000;
		//		done.push_back(sensor);
		//	}
		//	for (const auto& [core, temp] : core_mapping) {
		//		if (cmp_less(core + 1, current_cpu.temp.size()) and cmp_less(temp, core_sensors.size())) {
		//			current_cpu.temp.at(core + 1).push_back(found_sensors.at(core_sensors.at(temp)).temp);
		//			if (current_cpu.temp.at(core + 1).size() > 20) current_cpu.temp.at(core + 1).pop_front();
		//		}
		//	}
		//}
	}

	auto get_core_mapping() -> unordered_flat_map<int, int> {
		unordered_flat_map<int, int> core_map;
		if (cpu_temp_only) return core_map;

		//? Try to get core mapping from /proc/cpuinfo
		//ifstream cpuinfo(Shared::procPath / "cpuinfo");
		//if (cpuinfo.good()) {
		//	int cpu, core, n = 0;
		//	for (string instr; cpuinfo >> instr;) {
		//		if (instr == "processor") {
		//			cpuinfo.ignore(SSmax, ':');
		//			cpuinfo >> cpu;
		//		}
		//		else if (instr.starts_with("core")) {
		//			cpuinfo.ignore(SSmax, ':');
		//			cpuinfo >> core;
		//			if (std::cmp_greater_equal(core, core_sensors.size())) {
		//				if (std::cmp_greater_equal(n, core_sensors.size())) n = 0;
		//				core_map[cpu] = n++;
		//			}
		//			else
		//				core_map[cpu] = core;
		//		}
		//		cpuinfo.ignore(SSmax, '\n');
		//	}
		//}

		////? If core mapping from cpuinfo was incomplete try to guess remainder, if missing completely, map 0-0 1-1 2-2 etc.
		//if (cmp_less(core_map.size(), Shared::coreCount)) {
		//	if (Shared::coreCount % 2 == 0 and (long)core_map.size() == Shared::coreCount / 2) {
		//		for (int i = 0, n = 0; i < Shared::coreCount / 2; i++) {
		//			if (std::cmp_greater_equal(n, core_sensors.size())) n = 0;
		//			core_map[Shared::coreCount / 2 + i] = n++;
		//		}
		//	}
		//	else {
		//		core_map.clear();
		//		for (int i = 0, n = 0; i < Shared::coreCount; i++) {
		//			if (std::cmp_greater_equal(n, core_sensors.size())) n = 0;
		//			core_map[i] = n++;
		//		}
		//	}
		//}

		////? Apply user set custom mapping if any
		//const auto& custom_map = Config::getS("cpu_core_map");
		//if (not custom_map.empty()) {
		//	try {
		//		for (const auto& split : ssplit(custom_map)) {
		//			const auto vals = ssplit(split, ':');
		//			if (vals.size() != 2) continue;
		//			int change_id = std::stoi(vals.at(0));
		//			int new_id = std::stoi(vals.at(1));
		//			if (not core_map.contains(change_id) or cmp_greater(new_id, core_sensors.size())) continue;
		//			core_map.at(change_id) = new_id;
		//		}
		//	}
		//	catch (...) {}
		//}

		return core_map;
	}

	struct battery {
		fs::path base_dir, energy_now, energy_full, power_now, status, online;
		string device_type;
		bool use_energy = true;
	};

	auto get_battery() -> tuple<int, long, string> {
		if (not has_battery) return {0, 0, ""};
		//static string auto_sel;
		//static unordered_flat_map<string, battery> batteries;

		////? Get paths to needed files and check for valid values on first run
		//if (batteries.empty() and has_battery) {
		//	if (fs::exists("/sys/class/power_supply")) {
		//		for (const auto& d : fs::directory_iterator("/sys/class/power_supply")) {
		//			//? Only consider online power supplies of type Battery or UPS
		//			//? see kernel docs for details on the file structure and contents
		//			//? https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-class-power
		//			battery new_bat;
		//			fs::path bat_dir;
		//			try {
		//				if (not d.is_directory()
		//					or not fs::exists(d.path() / "type")
		//					or not fs::exists(d.path() / "present")
		//					or stoi(readfile(d.path() / "present")) != 1)
		//					continue;
		//				string dev_type = readfile(d.path() / "type");
		//				if (is_in(dev_type, "Battery", "UPS")) {
		//					bat_dir = d.path();
		//					new_bat.base_dir = d.path();
		//					new_bat.device_type = dev_type;
		//				}
		//			} catch (...) {
		//				//? skip power supplies not conforming to the kernel standard
		//				continue;
		//			}

		//			if (fs::exists(bat_dir / "energy_now")) new_bat.energy_now = bat_dir / "energy_now";
		//			else if (fs::exists(bat_dir / "charge_now")) new_bat.energy_now = bat_dir / "charge_now";
		//			else new_bat.use_energy = false;

		//			if (fs::exists(bat_dir / "energy_full")) new_bat.energy_full = bat_dir / "energy_full";
		//			else if (fs::exists(bat_dir / "charge_full")) new_bat.energy_full = bat_dir / "charge_full";
		//			else new_bat.use_energy = false;

		//			if (not new_bat.use_energy and not fs::exists(bat_dir / "capacity")) {
		//				continue;
		//			}

		//			if (fs::exists(bat_dir / "power_now")) new_bat.power_now = bat_dir / "power_now";
		//			else if (fs::exists(bat_dir / "current_now")) new_bat.power_now = bat_dir / "current_now";

		//			if (fs::exists(bat_dir / "AC0/online")) new_bat.online = bat_dir / "AC0/online";
		//			else if (fs::exists(bat_dir / "AC/online")) new_bat.online = bat_dir / "AC/online";

		//			batteries[bat_dir.filename()] = new_bat;
		//			Config::available_batteries.push_back(bat_dir.filename());
		//		}
		//	}
		//	if (batteries.empty()) {
		//		has_battery = false;
		//		return {0, 0, ""};
		//	}
		//}

		//auto& battery_sel = Config::getS("selected_battery");

		//if (auto_sel.empty()) {
		//	for (auto& [name, bat] : batteries) {
		//		if (bat.device_type == "Battery") {
		//			auto_sel = name;
		//			break;
		//		}
		//	}
		//	if (auto_sel.empty()) auto_sel = batteries.begin()->first;
		//}

		//auto& b = (battery_sel != "Auto" and batteries.contains(battery_sel) ? batteries.at(battery_sel) : batteries.at(auto_sel));

		//int percent = -1;
		//long seconds = -1;

		////? Try to get battery percentage
		//if (b.use_energy) {
		//	try {
		//		percent = round(100.0 * stoll(readfile(b.energy_now, "-1")) / stoll(readfile(b.energy_full, "1")));
		//	}
		//	catch (const std::invalid_argument&) { }
		//	catch (const std::out_of_range&) { }
		//}
		//if (percent < 0) {
		//	try {
		//		percent = stoll(readfile(b.base_dir / "capacity", "-1"));
		//	}
		//	catch (const std::invalid_argument&) { }
		//	catch (const std::out_of_range&) { }
		//}
		//if (percent < 0) {
		//	has_battery = false;
		//	return {0, 0, ""};
		//}

		////? Get charging/discharging status
		//string status = str_to_lower(readfile(b.base_dir / "status", "unknown"));
		//if (status == "unknown" and not b.online.empty()) {
		//	const auto online = readfile(b.online, "0");
		//	if (online == "1" and percent < 100) status = "charging";
		//	else if (online == "1") status = "full";
		//	else status = "discharging";
		//}

		////? Get seconds to empty
		//if (not is_in(status, "charging", "full")) {
		//	if (b.use_energy and not b.power_now.empty()) {
		//		try {
		//			seconds = round((double)stoll(readfile(b.energy_now, "0")) / stoll(readfile(b.power_now, "1")) * 3600);
		//		}
		//		catch (const std::invalid_argument&) { }
		//		catch (const std::out_of_range&) { }
		//	}
		//	if (seconds < 0 and fs::exists(b.base_dir / "time_to_empty")) {
		//		try {
		//			seconds = stoll(readfile(b.base_dir / "time_to_empty", "0")) * 60;
		//		}
		//		catch (const std::invalid_argument&) { }
		//		catch (const std::out_of_range&) { }
		//	}
		//}

		//return {percent, seconds, status};

		return { 0, 0, "" };
	}

	auto collect(const bool no_update) -> cpu_info& {
		if (Runner::stopping or (no_update and not current_cpu.cpu_percent.at("total").empty())) return current_cpu;
		auto& cpu = current_cpu;

		if (has_gpu and Config::getB("show_gpu")) {
			std::lock_guard lck(Cpu::SMImutex);
			SMI_trigger();
			gpu_clock = GpuRawStats.clock_mhz;
			cpu.gpu_temp.push_back(GpuRawStats.temp);
			if (cpu.gpu_temp.size() > 40) cpu.gpu_temp.pop_front();
			cpu.cpu_percent.at("gpu").push_back(GpuRawStats.usage);
			while (cmp_greater(cpu.cpu_percent.at("gpu").size(), width * 2)) cpu.cpu_percent.at("gpu").pop_front();
		}

		cpuHz = get_cpuHz();
	
		cpu.load_avg[0] = Cpu::load_avg_1m;
		cpu.load_avg[1] = Cpu::load_avg_5m;
		cpu.load_avg[2] = Cpu::load_avg_15m;

		vector<_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> sppi(Shared::coreCount);
		if (not NT_SUCCESS(
				NtQuerySystemInformation(SystemProcessorPerformanceInformation,
				&sppi[0],
				Shared::coreCount * sizeof(_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION),
				NULL))){
			throw std::runtime_error("Failed to run Cpu::collect() -> NtQuerySystemInformation()");
		}

		vector<long long> idle, kernel, user, interrupt, dpc, total;
		long long totals;
		long long cpu_total = 0;

		//? Usage for each core
		for (int i = 0; i < Shared::coreCount; i++) {
			user.push_back(sppi[i].UserTime.QuadPart);
			idle.push_back(sppi[i].IdleTime.QuadPart);
			kernel.push_back(sppi[i].KernelTime.QuadPart - idle.back());
			dpc.push_back(sppi[i].Reserved1[0].QuadPart);
			interrupt.push_back(sppi[i].Reserved1[1].QuadPart);
			

			totals = 0;
			for (auto& v : { kernel, user, dpc, interrupt, idle }) totals += v.back();
			//for (auto& v : { idle, dpc, interrupt }) idles += v.back();

			const long long calc_totals = max(0ll, totals - core_old_totals.at(i));
			const long long calc_idles = max(0ll, idle.back() - core_old_idles.at(i));
			core_old_totals.at(i) = totals;
			core_old_idles.at(i) = idle.back();

			cpu.core_percent.at(i).push_back(clamp((long long)round((double)(calc_totals - calc_idles) * 100 / calc_totals), 0ll, 100ll));
			cpu_total += cpu.core_percent.at(i).back();

			//? Reduce size if there are more values than needed for graph
			if (cpu.core_percent.at(i).size() > 40) cpu.core_percent.at(i).pop_front();

		}

		//? Usage accumulated for total cpu usage
		vector<long long> times;
		totals = 0;
		for (auto& v : { kernel, user, dpc, interrupt, idle}) {
			times.push_back(std::accumulate(v.cbegin(), v.cend(), 0));
			totals += times.back();
		}
		//totals = times.at(0) + times.at(1) + times.at(2) + times.at(3);
		
		
		const long long calc_totals = max(1ll, totals - cpu_old.at("totals"));
		cpu_old.at("totals") = totals;

		//? Total usage of cpu
		cpu.cpu_percent.at("total").push_back(clamp(cpu_total / Shared::coreCount, 0ll, 100ll));

		//? Reduce size if there are more values than needed for graph
		while (cmp_greater(cpu.cpu_percent.at("total").size(), width * 2)) cpu.cpu_percent.at("total").pop_front();

		//? Populate cpu.cpu_percent with all fields from stat
		for (int ii = 0; const auto& val : times) {
			cpu.cpu_percent.at(time_names.at(ii)).push_back(clamp((long long)round((double)(val - cpu_old.at(time_names.at(ii))) * 100 / calc_totals), 0ll, 100ll));
			cpu_old.at(time_names.at(ii)) = val;

			//? Reduce size if there are more values than needed for graph
			while (cmp_greater(cpu.cpu_percent.at(time_names.at(ii)).size(), width * 2)) cpu.cpu_percent.at(time_names.at(ii)).pop_front();
			ii++;
		}

	//	if (Config::getB("check_temp") and got_sensors)
	//		update_sensors();

		has_battery = false;
		got_sensors = false;
	//	if (Config::getB("show_battery") and has_battery)
	//		current_bat = get_battery();

		return cpu;
	}
}

namespace Mem {
	bool has_swap = false;
	vector<string> fstab;
	fs::file_time_type fstab_time;
	int disk_ios = 0;
	vector<string> last_found;
	int64_t totalMem = 0;

	mem_info current_mem {};

	int64_t get_totalMem() {
		MEMORYSTATUSEX memstat;
		memstat.dwLength = sizeof(MEMORYSTATUSEX);
		if (not GlobalMemoryStatusEx(&memstat)) {
			throw std::runtime_error("Failed to run Mem::collect() -> GlobalMemoryStatusEx()");
		}
		return static_cast<int64_t>(memstat.ullTotalPhys);
	}

	auto collect(const bool no_update) -> mem_info& {
		if (Runner::stopping or (no_update and not current_mem.percent.at("used").empty())) return current_mem;
		
		auto& show_swap = Config::getB("show_page");
		auto& show_disks = Config::getB("show_disks");
		auto& mem = current_mem;

		if (Cpu::has_gpu and Config::getB("show_gpu")) {
			std::lock_guard lck(Cpu::SMImutex);
			if (not Cpu::shown) Cpu::SMI_trigger();
			mem.stats.at("gpu_total") = Cpu::GpuRawStats.mem_total;
			mem.stats.at("gpu_used") = Cpu::GpuRawStats.mem_used;
			mem.stats.at("gpu_free") = mem.stats.at("gpu_total") - mem.stats.at("gpu_used");
			for (const auto name : { "gpu_used", "gpu_free" }) {
				mem.percent.at(name).push_back(round((double)mem.stats.at(name) * 100 / mem.stats.at("gpu_total")));
				while (cmp_greater(mem.percent.at(name).size(), width * 2)) mem.percent.at(name).pop_front();
			}
		}

		MEMORYSTATUSEX memstat;
		memstat.dwLength = sizeof(MEMORYSTATUSEX);
		PERFORMACE_INFORMATION perfinfo;
		
		if (not GlobalMemoryStatusEx(&memstat)) {
			throw std::runtime_error("Failed to run Mem::collect() -> GlobalMemoryStatusEx()");
		}
		if (not GetPerformanceInfo(&perfinfo, sizeof(PERFORMANCE_INFORMATION))) {
			throw std::runtime_error("Failed to run Mem::collect() -> GetPerformanceInfo()");
		}

		totalMem = static_cast<int64_t>(memstat.ullTotalPhys);
		const int64_t totalCommit = perfinfo.CommitLimit * perfinfo.PageSize;
		mem.stats.at("available") = static_cast<int64_t>(memstat.ullAvailPhys);
		mem.stats.at("used") = totalMem * memstat.dwMemoryLoad / 100;
		mem.stats.at("cached") = perfinfo.SystemCache * perfinfo.PageSize;
		mem.stats.at("commit") = perfinfo.CommitTotal * perfinfo.PageSize;

		mem.stats.at("page_total") = static_cast<int64_t>(memstat.ullTotalPageFile) - totalMem;
		mem.stats.at("page_free") = static_cast<int64_t>(memstat.ullAvailPageFile);
		if (mem.stats.at("page_total") < mem.stats.at("page_free")) {
			mem.stats.at("page_total") += mem.stats.at("page_free");
			mem.pagevirt = true;
		}
		else
			mem.pagevirt = false;
		mem.stats.at("page_used") = mem.stats.at("page_total") - mem.stats.at("page_free");

		//? Calculate percentages
		for (const string name : { "used", "available", "cached", "commit"}) {
			mem.percent.at(name).push_back(round((double)mem.stats.at(name) * 100 / (name == "commit" ? totalCommit : totalMem)));
			while (cmp_greater(mem.percent.at(name).size(), width * 2)) mem.percent.at(name).pop_front();
		}
		

		if (show_swap and mem.stats.at("page_total") > 0) {
			for (const auto name : {"page_used", "page_free"}) {
				mem.percent.at(name).push_back(round((double)mem.stats.at(name) * 100 / mem.stats.at("page_total")));
				while (cmp_greater(mem.percent.at(name).size(), width * 2)) mem.percent.at(name).pop_front();
			}
			has_swap = true;
		}
		else
			has_swap = false;

		//? Get disks stats
		if (show_disks) {
			uint64_t systime = GetTickCount64();
			auto free_priv = Config::getB("disk_free_priv");
			auto& disks_filter = Config::getS("disks_filter");
			bool filter_exclude = false;
			auto& only_physical = Config::getB("only_physical");
			auto& disks = mem.disks;
			disk_ios = 0;

			vector<string> filter;
			if (not disks_filter.empty()) {
				filter = ssplit(disks_filter);
				if (filter.at(0).starts_with("exclude=")) {
					filter_exclude = true;
					filter.at(0) = filter.at(0).substr(8);
				}
			}

			//? Get bitmask containing drives in use
			DWORD logical_drives = GetLogicalDrives();
			if (logical_drives == 0) return mem;
			
			vector<string> found;
			found.reserve(last_found.size());			
			for (int i = 0; i < 26; i++) {
				if (not (logical_drives & (1 << i))) continue;
				string letter = string(1, 'A' + i) + ":\\";
				
				//? Get device type and continue loop if unknown or failed
				UINT device_type = GetDriveTypeA(letter.c_str());
				if (device_type < 2) continue;

				//? Get name of drive
				array<char, MAX_PATH + 1> ch_name;
				GetVolumeInformationA(letter.c_str(), ch_name.data(), MAX_PATH + 1, 0, 0, 0, nullptr, 0);
				string name(ch_name.data());
				
				//? Match filter if not empty
				if (not filter.empty()) {
					bool match = v_contains(filter, letter) or (not name.empty() and v_contains(filter, name));
					if ((filter_exclude and match) or (not filter_exclude and not match))
						continue;
				}

				if (not only_physical or (only_physical and (device_type == DRIVE_FIXED or device_type == DRIVE_REMOVABLE))) {
					found.push_back(letter);

					if (not disks.contains(letter))
						disks[letter] = { name };
					else
						disks.at(letter).name = name;

					auto& disk = disks.at(letter);

					//? Get disk total size, free and used
					ULARGE_INTEGER freeBytesCaller, totalBytes, freeBytes;
					if (GetDiskFreeSpaceExA(letter.c_str(), &freeBytesCaller, &totalBytes, &freeBytes)) {
						disk.total = totalBytes.QuadPart;
						disk.free = (free_priv ? freeBytes.QuadPart : freeBytesCaller.QuadPart);
						disk.used = disk.total - disk.free;
						disk.used_percent = round((double)disk.used * 100 / disk.total);
						disk.free_percent = 100 - disk.used_percent;
					}

					//? Get disk IO
					//! Based on the method used in psutil
					//! see https://github.com/giampaolo/psutil/blob/master/psutil/arch/windows/disk.c
					HandleWrapper dHandle(CreateFileW(_bstr_t(string("\\\\.\\" + letter.substr(0, 2)).c_str()), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
					if (dHandle.valid) {
						DISK_PERFORMANCE diskperf;
						DWORD retSize = 0;
						DWORD getSize = sizeof(diskperf);
						BOOL status;
						for (int bx = 1; bx < 1024; bx++) {
							status = DeviceIoControl(dHandle(), IOCTL_DISK_PERFORMANCE, nullptr, 0, &diskperf, getSize, &retSize, nullptr);
							
							//* DeviceIoControl success
							if (status != 0) {
								disk_ios++;
								
								//? Read
								if (disk.io_read.empty())
									disk.io_read.push_back(0);
								else
									disk.io_read.push_back(max((int64_t)0, (diskperf.BytesRead.QuadPart - disk.old_io.at(0))));
								disk.old_io.at(0) = diskperf.BytesRead.QuadPart;
								while (cmp_greater(disk.io_read.size(), width * 2)) disk.io_read.pop_front();

								//? Write
								if (disk.io_write.empty())
									disk.io_write.push_back(0);
								else
									disk.io_write.push_back(max((int64_t)0, (diskperf.BytesWritten.QuadPart - disk.old_io.at(1))));
								disk.old_io.at(1) = diskperf.BytesWritten.QuadPart;
								while (cmp_greater(disk.io_write.size(), width * 2)) disk.io_write.pop_front();

								//? IO%
								int64_t io_time = diskperf.ReadTime.QuadPart + diskperf.WriteTime.QuadPart;
								if (disk.io_activity.empty())
									disk.io_activity.push_back(0);
								else
									disk.io_activity.push_back(clamp((long)round((double)(io_time - disk.old_io.at(2)) / 1000 / (systime - old_systime)), 0l, 100l));
								disk.old_io.at(2) = io_time;
								while (cmp_greater(disk.io_activity.size(), width * 2)) disk.io_activity.pop_front();
							}
							
							//! DeviceIoControl fail
							else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
								getSize *= 2;
								continue;
							}
							break;
						}
					}
				}
			}
			old_systime = systime;

			//? Remove disks no longer mounted or filtered out
			for (auto it = disks.begin(); it != disks.end();) {
				if (not v_contains(found, it->first)) {
					it = disks.erase(it);
					redraw = true;
				}
				else {
					it++;
				}
			}

			if (found.size() != last_found.size()) redraw = true;
			mem.disks_order.swap(found);
		}

	return mem;
	}

}

namespace Net {
	unordered_flat_map<string, net_info> current_net;
	net_info empty_net = {};
	vector<string> interfaces;
	vector<string> failed;
	string selected_iface;
	int errors = 0;
	unordered_flat_map<string, uint64_t> graph_max = { {"download", {}}, {"upload", {}} };
	unordered_flat_map<string, array<int, 2>> max_count = { {"download", {}}, {"upload", {}} };
	bool rescale = true;
	uint64_t timestamp = 0;

	auto collect(const bool no_update) -> net_info& {
		auto& net = current_net;

		auto& config_iface = Config::getS("net_iface");
		auto& net_sync = Config::getB("net_sync");
		auto& net_auto = Config::getB("net_auto");
		auto new_timestamp = time_ms();

		//! Much of the following code is based on the implementation used in psutil
		//! See: https://github.com/giampaolo/psutil/blob/master/psutil/arch/windows/net.c
		if (not no_update) {
			//? Get list of adapters
			ULONG bufSize = 0;
			if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, nullptr, &bufSize) != ERROR_BUFFER_OVERFLOW) {
				throw std::runtime_error("Net::collect() -> GetAdaptersAddresses() failed to get buffer size!");
			}
		
			auto adapters = std::unique_ptr<IP_ADAPTER_ADDRESSES, decltype(std::free)*>{reinterpret_cast<IP_ADAPTER_ADDRESSES*>(std::malloc(bufSize)), std::free};
			if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters.get(), &bufSize)) {
				throw std::runtime_error("Net::collect() -> GetAdaptersAddresses() failed to get adapter list!");
			}

			interfaces.clear();

			//? Iterate through list of adapters
			for (auto a = adapters.get(); a != nullptr; a = a->Next) {
				string iface = bstr2str(a->FriendlyName);
				interfaces.push_back(iface);
				net[iface].connected = (a->OperStatus == IfOperStatusUp);
			
				//? Get IP adresses associated with adapter
				bool ip4 = false, ip6 = false;
				for (auto u = a->FirstUnicastAddress; u != nullptr and not ip4; u = u->Next) {
					auto family = u->Address.lpSockaddr->sa_family;
					if (family == AF_INET and not ip4) {
						auto sa_in = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
						array<char, 256> ipAddress;
						if (inet_ntop(AF_INET, &sa_in->sin_addr, ipAddress.data(), 256) == NULL)
							continue;
						net[iface].ipv4 = string(ipAddress.data());
						ip4 = not net[iface].ipv4.empty();
					}
					else if (family == AF_INET6 and not ip6) {
						auto sa_in = reinterpret_cast<sockaddr_in6*>(u->Address.lpSockaddr);
						array<char, 256> ipAddress;
						if (inet_ntop(AF_INET6, &sa_in->sin6_addr, ipAddress.data(), 256) == NULL)
							continue;
						net[iface].ipv6 = string(ipAddress.data());
						ip6 = not net[iface].ipv6.empty();
					}
					else
						continue;
				}

				//? Get IO stats for adapter
				MIB_IF_ROW2 ifEntry;
				SecureZeroMemory((PVOID)&ifEntry, sizeof(MIB_IF_ROW2));
				ifEntry.InterfaceIndex = a->IfIndex;
				if (GetIfEntry2(&ifEntry) != NO_ERROR) {
					if (not v_contains(failed, iface)) {
						failed.push_back(iface);
						Logger::debug("Failed to get IO stats for network adapter: " + iface);
					}
					continue;
				}

				for (const string dir : {"download", "upload"}) {
					auto& saved_stat = net.at(iface).stat.at(dir);
					auto& bandwidth = net.at(iface).bandwidth.at(dir);

					uint64_t val = (dir == "download" ? ifEntry.InOctets : ifEntry.OutOctets);

					//? Update speed, total and top values
					if (val < saved_stat.last) {
						saved_stat.rollover += saved_stat.last;
						saved_stat.last = 0;
					}
					if (cmp_greater((unsigned long long)saved_stat.rollover + (unsigned long long)val, numeric_limits<uint64_t>::max())) {
						saved_stat.rollover = 0;
						saved_stat.last = 0;
					}
					saved_stat.speed = round((double)(val - saved_stat.last) / ((double)(new_timestamp - timestamp) / 1000));
					if (saved_stat.speed > saved_stat.top) saved_stat.top = saved_stat.speed;
					if (saved_stat.offset > val + saved_stat.rollover) saved_stat.offset = 0;
					saved_stat.total = (val + saved_stat.rollover) - saved_stat.offset;
					saved_stat.last = val;

					//? Add values to graph
					bandwidth.push_back(saved_stat.speed);
					while (cmp_greater(bandwidth.size(), width * 2)) bandwidth.pop_front();

					//? Set counters for auto scaling
					if (net_auto and selected_iface == iface) {
						if (saved_stat.speed > graph_max[dir]) {
							++max_count[dir][0];
							if (max_count[dir][1] > 0) --max_count[dir][1];
						}
						else if (graph_max[dir] > 10 << 10 and saved_stat.speed < graph_max[dir] / 10) {
							++max_count[dir][1];
							if (max_count[dir][0] > 0) --max_count[dir][0];
						}
					}
				}
			}
			
			timestamp = new_timestamp;

			//? Clean up net map if needed
			if (net.size() > interfaces.size()) {
				for (auto it = net.begin(); it != net.end();) {
					if (not v_contains(interfaces, it->first))
						it = net.erase(it);
					else
						it++;
				}
				net.compact();
			}
		}

		//? Return empty net_info struct if no interfaces was found
		if (net.empty())
			return empty_net;

		//? Find an interface to display if selected isn't set or valid
		if (selected_iface.empty() or not v_contains(interfaces, selected_iface)) {
			max_count["download"][0] = max_count["download"][1] = max_count["upload"][0] = max_count["upload"][1] = 0;
			redraw = true;
			if (net_auto) rescale = true;
			if (not config_iface.empty() and v_contains(interfaces, config_iface)) selected_iface = config_iface;
			else {
				//? Sort interfaces by total upload + download bytes
				auto sorted_interfaces = interfaces;
				rng::sort(sorted_interfaces, [&](const auto& a, const auto& b){
					return 	cmp_greater(net.at(a).stat["download"].total + net.at(a).stat["upload"].total,
										net.at(b).stat["download"].total + net.at(b).stat["upload"].total);
				});
				//? Try to set to a connected interface
				selected_iface.clear();
				for (const auto& iface : sorted_interfaces) {
					if (net.at(iface).connected) selected_iface = iface;
					break;
				}
				//? If no interface is connected set to first available
				if (selected_iface.empty() and not sorted_interfaces.empty()) selected_iface = sorted_interfaces.at(0);
				else if (sorted_interfaces.empty()) return empty_net;
			}
		}

		//? Calculate max scale for graphs if needed
		if (net_auto) {
			bool sync = false;
			for (const auto& dir: {"download", "upload"}) {
				for (const auto& sel : {0, 1}) {
					if (rescale or max_count[dir][sel] >= 5) {
						const uint64_t avg_speed = (net[selected_iface].bandwidth[dir].size() > 5
							? std::accumulate(net.at(selected_iface).bandwidth.at(dir).rbegin(), net.at(selected_iface).bandwidth.at(dir).rbegin() + 5, 0) / 5
							: net[selected_iface].stat[dir].speed);
						graph_max[dir] = max(uint64_t(avg_speed * (sel == 0 ? 1.3 : 3.0)), (uint64_t)10 << 10);
						max_count[dir][0] = max_count[dir][1] = 0;
						redraw = true;
						if (net_sync) sync = true;
						break;
					}
				}
				//? Sync download/upload graphs if enabled
				if (sync) {
					const auto other = (string(dir) == "upload" ? "download" : "upload");
					graph_max[other] = graph_max[dir];
					max_count[other][0] = max_count[other][1] = 0;
					break;
				}
			}
		}

		rescale = false;
		return net.at(selected_iface);
	}
}

namespace Proc {

	vector<proc_info> current_procs;
	vector<proc_info> current_svcs;
	bool services_swap = false;
	unordered_flat_map<string, string> uid_user;
	string current_sort;
	string current_filter;
	bool current_rev = false;

	fs::file_time_type passwd_time;

	uint64_t cputimes = 0;
	int collapse = -1, expand = -1;
	uint64_t old_cputimes = 0;
	atomic<int> numpids = 0;
	int filter_found = 0;

	detail_container detailed;

	struct tree_proc {
		std::reference_wrapper<proc_info> entry;
		vector<tree_proc> children;
	};

	void proc_sorter(vector<proc_info>& proc_vec, string sorting, const bool reverse, const bool tree = false, const bool services = false) {
		if (services and sorting == "service") sorting = "program";
		else if (services and sorting == "caption") sorting = "command";
		else if (services and sorting == "status") sorting = "user";
		if (reverse) {
			switch (v_index(sort_vector, sorting)) {
			case 0: rng::stable_sort(proc_vec, rng::less{}, &proc_info::pid); 		break;
			case 1: rng::stable_sort(proc_vec, rng::less{}, &proc_info::name);		break;
			case 2: rng::stable_sort(proc_vec, rng::less{}, &proc_info::cmd); 		break;
			case 3: rng::stable_sort(proc_vec, rng::less{}, &proc_info::threads);	break;
			case 4: rng::stable_sort(proc_vec, rng::less{}, &proc_info::user);		break;
			case 5: rng::stable_sort(proc_vec, rng::less{}, &proc_info::mem); 		break;
			case 6: rng::stable_sort(proc_vec, rng::less{}, &proc_info::cpu_p);		break;
			case 7: rng::stable_sort(proc_vec, rng::less{}, &proc_info::cpu_c);		break;
			}
		}
		else {
			switch (v_index(sort_vector, sorting)) {
			case 0: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::pid); 		break;
			case 1: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::name);		break;
			case 2: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::cmd); 		break;
			case 3: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::threads);	break;
			case 4: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::user); 		break;
			case 5: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::mem); 		break;
			case 6: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::cpu_p);   	break;
			case 7: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::cpu_c);   	break;
			}
		}

		//* When sorting with "cpu lazy" push processes over threshold cpu usage to the front regardless of cumulative usage
		if (not tree and not reverse and sorting == "cpu lazy") {
			double max = 10.0, target = 30.0;
			for (size_t i = 0, x = 0, offset = 0; i < proc_vec.size(); i++) {
				if (i <= 5 and proc_vec.at(i).cpu_p > max)
					max = proc_vec.at(i).cpu_p;
				else if (i == 6)
					target = (max > 30.0) ? max : 10.0;
				if (i == offset and proc_vec.at(i).cpu_p > 30.0)
					offset++;
				else if (proc_vec.at(i).cpu_p > target) {
					rotate(proc_vec.begin() + offset, proc_vec.begin() + i, proc_vec.begin() + i + 1);
					if (++x > 10) break;
				}
			}
		}
	}

	void tree_sort(vector<tree_proc>& proc_vec, const string& sorting, const bool reverse, int& c_index, const int index_max, const bool collapsed = false) {
		if (proc_vec.size() > 1) {
			if (reverse) {
				switch (v_index(sort_vector, sorting)) {
				case 3: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().threads < b.entry.get().threads; });	break;
				case 5: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().mem < b.entry.get().mem; });	break;
				case 6: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().cpu_p < b.entry.get().cpu_p; });	break;
				case 7: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().cpu_c < b.entry.get().cpu_c; });	break;
				}
			}
			else {
				switch (v_index(sort_vector, sorting)) {
				case 3: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().threads > b.entry.get().threads; });	break;
				case 5: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().mem > b.entry.get().mem; });	break;
				case 6: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().cpu_p > b.entry.get().cpu_p; });	break;
				case 7: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().cpu_c > b.entry.get().cpu_c; });	break;
				}
			}
		}

		for (auto& r : proc_vec) {
			r.entry.get().tree_index = (collapsed or r.entry.get().filtered ? index_max : c_index++);
			if (not r.children.empty()) {
				tree_sort(r.children, sorting, reverse, c_index, (collapsed or r.entry.get().collapsed or r.entry.get().tree_index == index_max));
			}
		}
	}

	//* Generate process tree list
	void _tree_gen(proc_info& cur_proc, vector<proc_info>& in_procs, vector<tree_proc>& out_procs, int cur_depth, const bool collapsed, const string& filter, bool found=false, const bool no_update=false, const bool should_filter=false) {
		auto cur_pos = out_procs.size();
		bool filtering = false;

		//? If filtering, include children of matching processes
		if (not found and (should_filter or not filter.empty())) {
			if (not s_contains(std::to_string(cur_proc.pid), filter)
			and not s_contains(cur_proc.name, filter)
			and not s_contains(cur_proc.cmd, filter)
			and not s_contains(cur_proc.user, filter)) {
				filtering = true;
				cur_proc.filtered = true;
				filter_found++;
			}
			else {
				found = true;
				cur_depth = 0;
			}
		}
		else if (cur_proc.filtered) cur_proc.filtered = false;

		cur_proc.depth = cur_depth;

		//? Set tree index position for process if not filtered out or currently in a collapsed sub-tree
		out_procs.push_back({ cur_proc });
		if (not collapsed and not filtering) {
			cur_proc.tree_index = out_procs.size() - 1;
			
			//? Try to find name of the binary file and append to program name if not the same
			if (cur_proc.short_cmd.empty() and WMIList.contains(cur_proc.pid)) {
				string pname = bstr2str(WMIList.at(cur_proc.pid).Name);
				if (pname.size() < cur_proc.cmd.size()) {
					std::string_view cmd = cur_proc.cmd;
					auto ssfind = cmd.find(pname);
					if (ssfind + pname.size() < cmd.size()) {
						cmd.remove_prefix(ssfind + pname.size());
						if (cmd.starts_with(pname)) cmd.remove_prefix(pname.size());
						if (cmd.starts_with("\"")) cmd.remove_prefix(1);
						if (cmd.starts_with(" ")) cmd.remove_prefix(1);
						cur_proc.short_cmd = string(cmd);
					}
				}

				if (cur_proc.short_cmd.empty())
					cur_proc.short_cmd = pname;
			}
		}
		else {
			cur_proc.tree_index = in_procs.size();
		}

		//? Recursive iteration over all children
		int children = 0;
		for (auto& p : rng::equal_range(in_procs, cur_proc.pid, rng::less{}, &proc_info::ppid)) {
			if (collapsed and not filtering) {
				cur_proc.filtered = true;
			}
			children++;
			
			_tree_gen(p, in_procs, out_procs.back().children, cur_depth + 1, (collapsed or cur_proc.collapsed), filter, found, no_update, should_filter);
			
			if (not no_update and not filtering and (collapsed or cur_proc.collapsed)) {
				//auto& parent = cur_proc;
				cur_proc.cpu_p += p.cpu_p;
				cur_proc.cpu_c += p.cpu_c;
				cur_proc.mem += p.mem;
				cur_proc.threads += p.threads;
				filter_found++;
				p.filtered = true;
			}
		}
		if (collapsed or filtering) {
			return;
		}

		//? Add tree terminator symbol if it's the last child in a sub-tree
		if (children > 0 and not out_procs.back().children.back().entry.get().prefix.ends_with("]─"))
			out_procs.back().children.back().entry.get().prefix.replace(out_procs.back().children.back().entry.get().prefix.size() - 8, 8, " └─ ");

		//? Add collapse/expand symbols if process have any children
		out_procs.at(cur_pos).entry.get().prefix = " │ "s * cur_depth + (children > 0 ? (cur_proc.collapsed ? "[+]─" : "[-]─") : " ├─ ");
	}

	//* Get detailed info for selected process
	void _collect_details(const size_t pid, const string name, const uint64_t uptime, vector<proc_info>& procs, uint64_t totalMem) {
		const auto& services = Config::getB("proc_services");
		if (pid != detailed.last_pid or name != detailed.last_name) {
			detailed = {};
			detailed.last_pid = pid;
			detailed.last_name = name;
			detailed.status = "Running";
		}

		if (services and WMISvcList.contains(name)) {
			const auto& svc = WMISvcList.at(name);
			detailed.status = bstr2str(svc.State);
			detailed.owner = bstr2str(svc.Owner);
			detailed.start = bstr2str(svc.StartMode);
			detailed.description = bstr2str(svc.Description);
			detailed.can_pause = svc.AcceptPause;
			detailed.can_stop = svc.AcceptStop;
		}

		if (detailed.status == "Running") {

			//? Copy proc_info for process from proc vector
			auto p_info = (services ? rng::find(procs, name, &proc_info::name) : rng::find(procs, pid, &proc_info::pid));
			detailed.entry = *p_info;

			//? Update cpu percent deque for process cpu graph
			if (not Config::getB("proc_per_core")) detailed.entry.cpu_p *= Shared::coreCount;
			detailed.cpu_percent.push_back(clamp((long long)round(detailed.entry.cpu_p), 0ll, 100ll));
			while (cmp_greater(detailed.cpu_percent.size(), width)) detailed.cpu_percent.pop_front();

			//? Process runtime
			if (detailed.entry.cpu_s > 0) {
				detailed.elapsed = sec_to_dhms((uptime - detailed.entry.cpu_s) / 10'000'000);
				if (detailed.elapsed.size() > 8) detailed.elapsed.resize(detailed.elapsed.size() - 3);
			}
			else {
				detailed.elapsed = "unknown";
			}

			detailed.mem_bytes.push_back(detailed.entry.mem);
			detailed.mem_percent = (double)detailed.entry.mem * 100 / totalMem;
			detailed.memory = floating_humanizer(detailed.entry.mem);

			if (detailed.first_mem == -1 or detailed.first_mem < detailed.mem_bytes.back() / 2 or detailed.first_mem > detailed.mem_bytes.back() * 4) {
				detailed.first_mem = min(detailed.mem_bytes.back() * 2, (long long)totalMem);
				redraw = true;
			}

			while (cmp_greater(detailed.mem_bytes.size(), width)) detailed.mem_bytes.pop_front();

			//? Get bytes read and written
			if (WMIList.contains(pid)) {
				detailed.io_read = floating_humanizer(_wtoi64(WMIList.at(pid).ReadTransferCount));
				detailed.io_write = floating_humanizer(_wtoi64(WMIList.at(pid).WriteTransferCount));
				Proc::WMI_requests.push_back(pid);
			}

			//? Get parent process name
			if (not services and detailed.parent.empty()) {
				auto p_entry = rng::find(procs, detailed.entry.ppid, &proc_info::pid);
				if (p_entry != procs.end()) detailed.parent = p_entry->name;
			}
		}
		else {
			detailed.entry = {};
			detailed.entry.name = name;
		}

	}



	//* Collects process information
	auto collect(const bool no_update) -> vector<proc_info>& {
		const auto& services = Config::getB("proc_services");
		const auto& sorting = (services ? Config::getS("services_sorting") : Config::getS("proc_sorting"));
		const auto& reverse = Config::getB("proc_reversed");
		const auto& filter = Config::getS("proc_filter");
		const auto& per_core = Config::getB("proc_per_core");
		const bool tree = (not services and Config::getB("proc_tree"));
		const auto& show_detailed = Config::getB("show_detailed");
		const auto& detailed_pid = Config::getI("detailed_pid");
		const auto& detailed_name = Config::getS("detailed_name");
		bool should_filter = current_filter != filter;
		if (should_filter) current_filter = filter;
		const bool sorted_change = (sorting != current_sort or reverse != current_rev or should_filter);
		if (sorted_change) {
			current_sort = sorting;
			current_rev = reverse;
		}
		int64_t totalMem = 0;

		FILETIME st;
		::GetSystemTimeAsFileTime(&st);
		const uint64_t systime = ULARGE_INTEGER{ st.dwLowDateTime, st.dwHighDateTime }.QuadPart;
		std::lock_guard<std::mutex> lck(WMImutex);

		const int cmult = (per_core) ? Shared::coreCount : 1;
		bool got_detailed = false;

		static vector<size_t> found;

		//* Use pids from last update if only changing filter, sorting or tree options
		if (no_update and not current_procs.empty()) {
			if (show_detailed and (detailed_pid != detailed.last_pid or detailed_name != detailed.last_name)) {
				_collect_details(detailed_pid, detailed_name, systime, (services ? current_svcs : current_procs), Mem::get_totalMem());
			}
		}
		//* ---------------------------------------------Collection start----------------------------------------------
		else {
			should_filter = true;
			totalMem = Mem::get_totalMem();

			//? Get cpu total times
			if (FILETIME idle, kernel, user; GetSystemTimes(&idle, &kernel, &user)) {
				cputimes	= ULARGE_INTEGER{ kernel.dwLowDateTime, kernel.dwHighDateTime }.QuadPart
							- ULARGE_INTEGER{ idle.dwLowDateTime, idle.dwHighDateTime }.QuadPart
							+ ULARGE_INTEGER{ user.dwLowDateTime, user.dwHighDateTime }.QuadPart;
			}
			else {
				throw std::runtime_error("Proc::collect() -> GetSystemTimes() failed!");
			}
			
			//? Iterate over all processes
			found.clear();
			HandleWrapper pSnap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));

			if (not pSnap.valid) {
				Logger::warning("Proc::collect() -> CreateToolhelp32Snapshot() failed!");
				return current_procs;
			}

			PROCESSENTRY32 pe;
			pe.dwSize = sizeof(PROCESSENTRY32);

			if (not Process32First(pSnap(), &pe)) {
				Logger::warning("Proc::collect() -> Process32First() failed!");
				return current_procs;
			}

			do {
				if (Runner::stopping) {
					if (not Proc::WMI_requests.empty()) Proc::WMI_trigger();
					return current_procs;
				}

				const size_t pid = pe.th32ProcessID;
				if (pid == 0) continue;
				HandleWrapper pHandle(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID));
				const bool hasWMI = WMIList.contains(pid);
				bool wmi_request = (not hasWMI and not Proc::WMI_running);
				
				found.push_back(pid);

				//? Check if pid already exists in current_procs
				auto find_old = rng::find(current_procs, pid, &proc_info::pid);
				bool no_cache = false;
				if (find_old == current_procs.end()) {
					current_procs.push_back({pid});
					find_old = current_procs.end() - 1;
					no_cache = true;
				}

				auto& new_proc = *find_old;

				//? Cache values that shouldn't change
				if (no_cache or (hasWMI and not new_proc.WMI)) {
					new_proc.name = bstr2str(pe.szExeFile);
					new_proc.ppid = pe.th32ParentProcessID;

					if (hasWMI) {
						if (new_proc.name.empty()) new_proc.name = bstr2str(WMIList.at(pid).Name);
						if (new_proc.ppid == 0) new_proc.ppid = WMIList.at(pid).ParentProcessId;
						new_proc.cmd = bstr2str(WMIList.at(pid).CommandLine);
						if (new_proc.cmd.empty())
							new_proc.cmd = bstr2str(WMIList.at(pid).ExecutablePath);
					}
					if (new_proc.cmd.empty()) new_proc.cmd = new_proc.name;

					new_proc.name = new_proc.name.substr(0, new_proc.name.find_last_of('.'));

					if (pHandle.valid) {
						HandleWrapper pToken{};
						if (OpenProcessToken(pHandle.wHandle, TOKEN_QUERY, &pToken.wHandle)) {
							DWORD dwLength = 0;
							GetTokenInformation(pToken.wHandle, TokenUser, nullptr, 0, &dwLength);
							if (dwLength > 0) {
								std::unique_ptr<BYTE[]> ptu(new BYTE[dwLength]);
								if (ptu != nullptr and GetTokenInformation(pToken.wHandle, TokenUser, ptu.get(), dwLength, &dwLength)) {
									SID_NAME_USE SidType;
									wchar_t lpName[260];
									wchar_t lpDomain[260];
									DWORD dwSize = 260;
									if (LookupAccountSid(0, ((PTOKEN_USER)ptu.get())->User.Sid, lpName, &dwSize, lpDomain, &dwSize, &SidType)) {
										new_proc.user = bstr2str(lpName);
										if (new_proc.user.empty())
											new_proc.user = bstr2str(lpDomain);
									}
								}
							}
						}
					}
					if (new_proc.user.empty() and pid < 1000) new_proc.user = "SYSTEM";
					new_proc.WMI = hasWMI;
				}

				//? Use parent process username if empty
				if (not no_cache and new_proc.user.empty()) {
					if (new_proc.ppid != 0) {
						if (auto parent = rng::find(current_procs, new_proc.ppid, &proc_info::pid); parent != current_procs.end()) {
							new_proc.user = parent->user;
						}
					}
					else
						new_proc.user = "SYSTEM";

					if (new_proc.user.empty()) new_proc.user = "******";
				}

				new_proc.threads = pe.cntThreads;

				uint64_t cpu_t = 0;
				if (pHandle.valid) {
					//? Process memory
					if (PROCESS_MEMORY_COUNTERS_EX pmem; GetProcessMemoryInfo(pHandle(), (PROCESS_MEMORY_COUNTERS *)&pmem, sizeof(PROCESS_MEMORY_COUNTERS_EX))) {
						new_proc.mem = pmem.PrivateUsage;
					}
					
					//? Process cpu stats
					if (FILETIME createT, exitT, kernelT, userT; GetProcessTimes(pHandle(), &createT, &exitT, &kernelT, &userT)) {
						new_proc.cpu_s = ULARGE_INTEGER{ createT.dwLowDateTime, createT.dwHighDateTime }.QuadPart;
						cpu_t = ULARGE_INTEGER{ kernelT.dwLowDateTime, kernelT.dwHighDateTime }.QuadPart + ULARGE_INTEGER{ userT.dwLowDateTime, userT.dwHighDateTime }.QuadPart;
					}
				}

				//? Process memory fallback to background WMI thread
				if (new_proc.mem == 0 and hasWMI) {
					new_proc.mem = _wtoi64(WMIList.at(pid).PrivateMemory);
					wmi_request = true;
				}

				//? Process cpu stats fallback to background WMI thread, will be inacurate when update timer is lower than 1 second
				if ((cpu_t == 0 or new_proc.cpu_s == 0) and hasWMI) {

					//? Convert process creation CIM_DATETIME to FILETIME, (less accurate than GetProcessTimes() due to loss of microsecond count)
					if (new_proc.cpu_s == 0) {
						const string strdate = bstr2str(WMIList.at(pid).CreationDate);
						if (strdate.size() > 18) {
							SYSTEMTIME t = { 0 };
							t.wYear = stoi(strdate.substr(0, 4));
							t.wMonth = stoi(strdate.substr(4, 2));
							t.wDay = stoi(strdate.substr(6, 2));
							t.wHour = stoi(strdate.substr(8, 2));
							t.wMinute = stoi(strdate.substr(10, 2));
							t.wSecond = stoi(strdate.substr(12, 2));
							t.wMilliseconds = stoi(strdate.substr(15, 3));
							ULARGE_INTEGER ft;
							if (SystemTimeToFileTime(&t, (LPFILETIME)&ft))
								new_proc.cpu_s = ft.QuadPart;
						}
					}

					//? Process cpu times
					cpu_t = _wtoi64(WMIList.at(pid).KernelModeTime) + _wtoi64(WMIList.at(pid).UserModeTime);

					wmi_request = true;
				}
				
				if (cpu_t != 0) {
					if (new_proc.cpu_t == 0) new_proc.cpu_t = cpu_t;
					
					//? Process cpu usage since last update
					new_proc.cpu_p = clamp(round(cmult * 100 * (cpu_t - new_proc.cpu_t) / max((uint64_t)1, cputimes - old_cputimes)) / 10.0, 0.0, 100.0 * Shared::coreCount);

					//? Process cumulative cpu usage since process start
					new_proc.cpu_c = (double)cpu_t / max(1ull, systime - new_proc.cpu_s);

					//? Update cached value with latest cpu times
					new_proc.cpu_t = cpu_t;
				}

				if (show_detailed and not got_detailed and new_proc.pid == detailed_pid) {
					got_detailed = true;
				}

				if (wmi_request) Proc::WMI_requests.push_back(pid);

			} while (Process32Next(pSnap(), &pe));

			//? Clear dead processes from current_procs
			auto eraser = rng::remove_if(current_procs, [&](const auto& element){ return not v_contains(found, element.pid); });
			current_procs.erase(eraser.begin(), eraser.end());

			//? Update the details info box for process if active
			if (not services and show_detailed and got_detailed) {
				_collect_details(detailed_pid, detailed_name, systime, current_procs, totalMem);
			}
			else if (show_detailed and not got_detailed and detailed.status != "Stopped") {
				detailed.status = "Stopped";
				redraw = true;
			}

			old_cputimes = cputimes;
		}
		
		//* Collect info for services using WMI if currently enabled
		if (services and not no_update) {
			bool got_detailed = false;
			for (const auto& [name, svc] : WMISvcList) {
				
				//? Check if pid already exists in current_svcs
				auto find_old = rng::find(current_svcs, name, &proc_info::name);
				if (find_old == current_svcs.end()) {
					current_svcs.push_back({});
					find_old = current_svcs.end() - 1;
				}

				auto& new_svc = *find_old;

				if (name == detailed_name) {
					got_detailed = true;
				}

				new_svc.name = name;
				new_svc.pid = svc.ProcessID;
				new_svc.cmd = bstr2str(svc.Caption);
				new_svc.user = bstr2str(svc.State);
				if (tree) new_svc.short_cmd = new_svc.cmd;

				//? Find pid entry in current_procs
				if (auto proc = rng::find(current_procs, new_svc.pid, &proc_info::pid); proc != current_procs.end()) {
					new_svc.cpu_c = proc->cpu_c;
					new_svc.cpu_p = proc->cpu_p;
					new_svc.cpu_s = proc->cpu_s;
					new_svc.mem = proc->mem;
					new_svc.threads = proc->threads;
				}

			}

			// ? Update the details info box for service if active
			if (show_detailed and got_detailed) {
				_collect_details(detailed_pid, detailed_name, systime, current_svcs, totalMem);
			}
			else if (show_detailed and not got_detailed and detailed.status != "Stopped") {
				detailed.status = "Stopped";
				redraw = true;
			}

			//? Clear missing services from current_svcs
			auto eraser = rng::remove_if(current_svcs, [&](const auto& element) { return not WMISvcList.contains(element.name); });
			current_svcs.erase(eraser.begin(), eraser.end());
		}

		//* ---------------------------------------------Collection done-----------------------------------------------

		auto& out_vec = (services ? current_svcs : current_procs);

		//* Match filter if defined
		if (should_filter) {
			filter_found = 0;
			for (auto& p : out_vec) {
				if (not tree and not filter.empty()) {
						if (not s_contains(to_string(p.pid), filter)
						and not s_contains(p.name, filter)
						and not s_contains(p.cmd, filter)
						and not s_contains(p.user, filter)) {
							p.filtered = true;
							filter_found++;
							}
						else {
							p.filtered = false;
						}
					}
				else {
					p.filtered = false;
				}
			}
		}

		//? Sort processes
		if (sorted_change or not no_update) {
			proc_sorter(out_vec, sorting, reverse, tree, services);
		}

		//* Generate tree view if enabled
		if (tree and not services and (not no_update or should_filter or sorted_change)) {
			bool locate_selection = false;
			if (auto find_pid = (collapse != -1 ? collapse : expand); find_pid != -1) {
				auto collapser = rng::find(out_vec, find_pid, &proc_info::pid);
				if (collapser != out_vec.end()) {
					if (collapse == expand) {
						collapser->collapsed = not collapser->collapsed;
					}
					else if (collapse > -1) {
						collapser->collapsed = true;
					}
					else if (expand > -1) {
						collapser->collapsed = false;
					}
					if (Config::ints.at("proc_selected") > 0) locate_selection = true;
				}
				collapse = expand = -1;
			}
			if (should_filter or not filter.empty()) filter_found = 0;

			vector<tree_proc> tree_procs;
			tree_procs.reserve(out_vec.size());

			for (auto& p : out_vec) {
				if (not v_contains(found, p.ppid)) p.ppid = 0;
			}

			//? Stable sort to retain selected sorting among processes with the same parent
			rng::stable_sort(out_vec, rng::less{}, & proc_info::ppid);

			//? Start recursive iteration over processes with the lowest shared parent pids
			for (auto& p : rng::equal_range(out_vec, out_vec.at(0).ppid, rng::less{}, &proc_info::ppid)) {
				_tree_gen(p, out_vec, tree_procs, 0, false, filter, false, no_update, should_filter);
			}

			//? Recursive sort over tree structure to account for collapsed processes in the tree
			int index = 0;
			tree_sort(tree_procs, sorting, reverse, index, out_vec.size());

			//? Add tree begin symbol to first item if childless
			if (tree_procs.front().children.empty())
				tree_procs.front().entry.get().prefix.replace(tree_procs.front().entry.get().prefix.size() - 8, 8, " ┌─ ");
			
			//? Add tree terminator symbol to last item if childless
			if (tree_procs.back().children.empty())
				tree_procs.back().entry.get().prefix.replace(tree_procs.back().entry.get().prefix.size() - 8, 8, " └─ ");

			//? Final sort based on tree index
			rng::sort(out_vec, rng::less{}, & proc_info::tree_index);

			//? Move current selection/view to the selected process when collapsing/expanding in the tree
			if (locate_selection) {
				int loc = rng::find(out_vec, Proc::selected_pid, &proc_info::pid)->tree_index;
				if (Config::ints.at("proc_start") >= loc or Config::ints.at("proc_start") <= loc - Proc::select_max)
					Config::ints.at("proc_start") = max(0, loc - 1);
				Config::ints.at("proc_selected") = loc - Config::ints.at("proc_start") + 1;
			}
		}

		numpids = (int)out_vec.size() - filter_found;
		if (not Proc::WMI_requests.empty()) Proc::WMI_trigger();
		return out_vec;
	}
}

namespace Tools {
	double system_uptime() {
		return (double)GetTickCount64() / 1000;
	}
}
