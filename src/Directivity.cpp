#include "Directivity.h"
#include "Settings.h"

#include <immintrin.h>
#include <phonon.h>
#include <shared_mutex>

namespace
{
	struct Vec3
	{
		float x, y, z;
	};

	struct Listener
	{
		Vec3  orientFront;
		Vec3  orientTop;
		Vec3  position;
		Vec3  velocity;
		void* cone;
	};

	struct Emitter
	{
		void*         cone;
		Vec3          orientFront;
		Vec3          orientTop;
		Vec3          position;
		Vec3          velocity;
		float         innerRadius;
		float         innerRadiusAngle;
		std::uint32_t channelCount;
		float         channelRadius;
		float*        channelAzimuths;
		void*         volumeCurve;
		void*         lfeCurve;
		void*         lpfDirectCurve;
		void*         lpfReverbCurve;
		void*         reverbCurve;
		float         curveDistanceScaler;
		float         dopplerScaler;
	};
	static_assert(offsetof(Emitter, position) == 0x20 && sizeof(Emitter) == 0x80);

	struct DSPSettings
	{
		float*        matrixCoefficients;
		float*        delayTimes;
		std::uint32_t srcChannelCount;
		std::uint32_t dstChannelCount;
		float         lpfDirect;
		float         lpfReverb;
		float         reverbLevel;
		float         dopplerFactor;
		float         emitterToListenerAngle;
		float         emitterToListenerDistance;
	};
	static_assert(offsetof(DSPSettings, emitterToListenerDistance) == 0x2C);

	constexpr std::ptrdiff_t kVoice = 0x128;
	constexpr std::ptrdiff_t kDSP = 0x130;
	constexpr std::ptrdiff_t kEmitter = 0x168;

	constexpr std::int32_t  kOk = 0;
	constexpr std::int32_t  kNoInterface = static_cast<std::int32_t>(0x80004002);
	constexpr std::int32_t  kOutOfMemory = static_cast<std::int32_t>(0x8007000E);
	constexpr std::int32_t  kFormatUnsupported = static_cast<std::int32_t>(0x88970001);
	constexpr std::uint32_t kXapoFlags = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;
	constexpr std::uint32_t kMaxChannels = 8;
	constexpr std::uint32_t kMusicFlag = 0x10;

	constexpr GUID kIUnknown{ 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
	constexpr GUID kIXAPO{ 0xA90BC001, 0xE897, 0xE897, { 0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x00 } };
	constexpr GUID kXAudio2{ 0x5a508685, 0xa254, 0x4fba, { 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf } };
	constexpr GUID kXAudio2Debug{ 0xdb05ea35, 0x0329, 0x4d4b, { 0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52 } };
	constexpr GUID kIXAudio2{ 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb } };

	bool Same(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

	using Voice = void;
	using CreateSourceVoice_t = HRESULT(__stdcall*)(void*, Voice**, const WAVEFORMATEX*, std::uint32_t, float, void*, const RE::XAUDIO2_VOICE_SENDS*, const RE::XAUDIO2_EFFECT_CHAIN*);
	using DestroyVoice_t = void(__stdcall*)(Voice*);
	using CoCreateInstance_t = HRESULT(__stdcall*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
	using Calculate_t = void (*)(const void*, const Listener*, const Emitter*, std::uint32_t, DSPSettings*);
	constexpr std::size_t kCreateSourceVoice = 8;
	constexpr std::size_t kDestroyVoice = 18;

	struct Phonon
	{
		decltype(&iplContextCreate)         contextCreate{ nullptr };
		decltype(&iplDirectEffectCreate)    directCreate{ nullptr };
		decltype(&iplDirectEffectApply)     directApply{ nullptr };
		decltype(&iplDirectEffectReset)     directReset{ nullptr };
		decltype(&iplDirectEffectRelease)   directRelease{ nullptr };
		decltype(&iplDirectivityCalculate)  directivity{ nullptr };
		IPLContext                          context{ nullptr };
	} g_phonon;

	void IPLCALL PhononLog(IPLLogLevel, const char* a_message)
	{
		logger::info("Steam Audio: {}", a_message ? a_message : "");
	}

	bool LoadPhonon()
	{
		HMODULE self{ nullptr };
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&LoadPhonon), &self);
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileNameW(self, buffer, MAX_PATH);
		const auto path = std::filesystem::path(buffer).parent_path() / L"SoundSourceDirectivity" / L"phonon.dll";
		const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!module) {
			logger::error("Could not load {} (error {}), nothing installed", path.string(), GetLastError());
			return false;
		}
		auto& p = g_phonon;
		p.contextCreate = reinterpret_cast<decltype(p.contextCreate)>(GetProcAddress(module, "iplContextCreate"));
		p.directCreate = reinterpret_cast<decltype(p.directCreate)>(GetProcAddress(module, "iplDirectEffectCreate"));
		p.directApply = reinterpret_cast<decltype(p.directApply)>(GetProcAddress(module, "iplDirectEffectApply"));
		p.directReset = reinterpret_cast<decltype(p.directReset)>(GetProcAddress(module, "iplDirectEffectReset"));
		p.directRelease = reinterpret_cast<decltype(p.directRelease)>(GetProcAddress(module, "iplDirectEffectRelease"));
		p.directivity = reinterpret_cast<decltype(p.directivity)>(GetProcAddress(module, "iplDirectivityCalculate"));
		if (!p.contextCreate || !p.directCreate || !p.directApply || !p.directReset || !p.directRelease || !p.directivity) {
			logger::error("phonon.dll is missing functions, nothing installed");
			return false;
		}
		IPLContextSettings settings{};
		settings.version = STEAMAUDIO_VERSION;
		settings.logCallback = PhononLog;
		settings.simdLevel = IPL_SIMDLEVEL_AVX2;
		if (p.contextCreate(&settings, &p.context) != IPL_STATUS_SUCCESS) {
			logger::error("Steam Audio context could not be created, nothing installed");
			return false;
		}
		logger::info("Steam Audio {}.{}.{} loaded from {}", STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, path.string());
		return true;
	}

	struct Stats
	{
		std::atomic<std::uint64_t> voices{ 0 }, others{ 0 }, refused{ 0 }, updates{ 0 }, applied{ 0 }, frames{ 0 };
		std::atomic<float>         lowest{ 1.0f };
		std::atomic<float>         largestStep{ 0.0f };
		double                     sum{ 0.0 };
		std::chrono::steady_clock::time_point last{ std::chrono::steady_clock::now() };
		std::unordered_set<std::uintptr_t> seen;
		int                        examples{ 0 };
	} g_stats;

	bool IsFloat(const RE::WAVEFORMATEX* a_format)
	{
		const auto& format = reinterpret_cast<const WAVEFORMATEX&>(*a_format);
		return format.wBitsPerSample == 32 && (format.wFormatTag == 3 || format.wFormatTag == 0xFFFE);
	}

	class Effect final : public RE::IXAPO
	{
	public:
		~Effect()
		{
			if (direct) {
				g_phonon.directRelease(&direct);
			}
		}

		std::int32_t QueryInterface(const REX::W32::GUID& a_iid, void** a_out) override
		{
			if (!a_out) {
				return kNoInterface;
			}
			const auto& iid = reinterpret_cast<const GUID&>(a_iid);
			if (Same(iid, kIUnknown) || Same(iid, kIXAPO)) {
				*a_out = this;
				AddRef();
				return kOk;
			}
			*a_out = nullptr;
			return kNoInterface;
		}

		std::uint32_t AddRef() override { return ++references; }

		std::uint32_t Release() override
		{
			const auto left = --references;
			if (left == 0) {
				delete this;
			}
			return left;
		}

		std::int32_t GetRegistrationProperties(RE::XAPO_REGISTRATION_PROPERTIES** a_properties) override
		{
			auto* properties = static_cast<RE::XAPO_REGISTRATION_PROPERTIES*>(CoTaskMemAlloc(sizeof(RE::XAPO_REGISTRATION_PROPERTIES)));
			if (!properties) {
				return kOutOfMemory;
			}
			std::memset(properties, 0, sizeof(*properties));
			wcscpy_s(properties->friendlyName, L"Sound Source Directivity");
			properties->majorVersion = 1;
			properties->flags = kXapoFlags;
			properties->minInputBufferCount = properties->maxInputBufferCount = 1;
			properties->minOutputBufferCount = properties->maxOutputBufferCount = 1;
			*a_properties = properties;
			return kOk;
		}

		std::int32_t IsInputFormatSupported(const RE::WAVEFORMATEX*, const RE::WAVEFORMATEX* a_requested, RE::WAVEFORMATEX** a_supported) override
		{
			if (a_supported) {
				*a_supported = nullptr;
			}
			return IsFloat(a_requested) ? kOk : kFormatUnsupported;
		}

		std::int32_t IsOutputFormatSupported(const RE::WAVEFORMATEX*, const RE::WAVEFORMATEX* a_requested, RE::WAVEFORMATEX** a_supported) override
		{
			if (a_supported) {
				*a_supported = nullptr;
			}
			return IsFloat(a_requested) ? kOk : kFormatUnsupported;
		}

		std::int32_t Initialize(const void*, std::uint32_t) override { return kOk; }

		void Reset() override { restart.store(true, std::memory_order_relaxed); }

		std::int32_t LockForProcess(std::uint32_t, const RE::XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* a_input, std::uint32_t, const RE::XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS*) override
		{
			const auto& in = reinterpret_cast<const WAVEFORMATEX&>(*a_input->format);
			const auto  rate = static_cast<std::int32_t>(in.nSamplesPerSec);
			const auto  count = static_cast<std::int32_t>(a_input->maxFrameCount);
			if (direct && (rate != sampleRate || count != frames || in.nChannels != channels)) {
				g_phonon.directRelease(&direct);
				direct = nullptr;
			}
			channels = in.nChannels;
			sampleRate = rate;
			frames = count;
			for (auto& c : planes) {
				c.assign(static_cast<std::size_t>(frames), 0.0f);
			}
			if (!direct && channels >= 1 && channels <= kMaxChannels) {
				IPLAudioSettings        audio{ sampleRate, frames };
				IPLDirectEffectSettings settings{ static_cast<IPLint32>(channels) };
				if (g_phonon.directCreate(g_phonon.context, &audio, &settings, &direct) != IPL_STATUS_SUCCESS) {
					direct = nullptr;
				}
			}
			static std::once_flag logged;
			std::call_once(logged, [&] { logger::info("First voice effect: {} ch, {} Hz, {} frames per pass, Steam direct effect {}", channels, sampleRate, frames, direct ? "made" : "FAILED"); });
			active = false;
			return kOk;
		}

		void UnlockForProcess() override {}

		void Process(std::uint32_t, const RE::XAPO_PROCESS_BUFFER_PARAMETERS* a_input, std::uint32_t, RE::XAPO_PROCESS_BUFFER_PARAMETERS* a_output, BOOL a_enabled) override
		{
			const auto valid = a_input->validFrameCount;
			auto*      out = static_cast<float*>(a_output->buffer);
			const auto* in = static_cast<const float*>(a_input->buffer);
			a_output->validFrameCount = valid;
			a_output->bufferFlags = a_input->bufferFlags;
			const bool silent = a_input->bufferFlags == RE::XAPO_BUFFER_FLAGS::XAPO_BUFFER_SILENT;
			if (in != out && !silent) {
				std::memcpy(out, in, static_cast<std::size_t>(valid) * channels * sizeof(float));
			}
			if (restart.exchange(false, std::memory_order_acq_rel)) {
				if (direct) {
					g_phonon.directReset(direct);
				}
				active = false;
				lastDb = 1.0f;
			}
			if (!a_enabled || !direct || valid > static_cast<std::uint32_t>(frames) || (!active && !directional.load(std::memory_order_acquire))) {
				return;
			}
			active = true;
			for (std::uint32_t c = 0; c < channels; ++c) {
				auto& plane = planes[c];
				for (std::uint32_t i = 0; i < valid; ++i) {
					plane[i] = silent ? 0.0f : out[i * channels + c];
				}
				std::fill(plane.begin() + valid, plane.end(), 0.0f);
			}
			float* data[kMaxChannels]{};
			for (std::uint32_t c = 0; c < channels; ++c) {
				data[c] = planes[c].data();
			}
			IPLDirectEffectParams params{};
			params.flags = IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY;
			params.directivity = gain.load(std::memory_order_relaxed);
			const bool measure = Settings::GetSingleton().log && !silent;
			double     before = 0.0;
			if (measure) {
				for (std::uint32_t c = 0; c < channels; ++c) {
					for (std::uint32_t i = 0; i < valid; ++i) {
						before += double(planes[c][i]) * planes[c][i];
					}
				}
			}
			IPLAudioBuffer buffer{ static_cast<IPLint32>(channels), frames, data };
			const auto     state = _mm_getcsr();
			_mm_setcsr(state | 0x8040);
			g_phonon.directApply(direct, &params, &buffer, &buffer);
			_mm_setcsr(state);
			if (!silent) {
				double after = 0.0;
				for (std::uint32_t c = 0; c < channels; ++c) {
					for (std::uint32_t i = 0; i < valid; ++i) {
						out[i * channels + c] = planes[c][i];
						after += double(planes[c][i]) * planes[c][i];
					}
				}
				if (measure && before > 1e-6) {
					const float db = static_cast<float>(10.0 * std::log10((std::max)(after / before, 1e-12)));
					if (lastDb < 1.0f) {
						const float step = std::fabs(db - lastDb);
						for (float seen = g_stats.largestStep.load(); step > seen && !g_stats.largestStep.compare_exchange_weak(seen, step);) {
						}
					}
					lastDb = db;
				} else {
					lastDb = 1.0f;
				}
			}
			g_stats.applied.fetch_add(1, std::memory_order_relaxed);
		}

		std::uint32_t CalcInputFrames(std::uint32_t a_frames) override { return a_frames; }
		std::uint32_t CalcOutputFrames(std::uint32_t a_frames) override { return a_frames; }

		std::atomic<float> gain{ 1.0f };
		std::atomic<bool>  directional{ false };
		std::atomic<bool>  restart{ false };

	private:
		std::atomic<std::uint32_t>       references{ 1 };
		IPLDirectEffect                  direct{ nullptr };
		std::uint32_t                    channels{ 0 };
		std::int32_t                     sampleRate{ 0 };
		std::int32_t                     frames{ 0 };
		bool                             active{ false };
		float                            lastDb{ 1.0f };
		std::array<std::vector<float>, kMaxChannels> planes;
	};

	std::shared_mutex                  g_voicesLock;
	std::unordered_map<Voice*, Effect*> g_voices;

	struct Track
	{
		Effect*                         effect{ nullptr };
		RE::NiPointer<RE::NiAVObject>   node;
		std::chrono::steady_clock::time_point seen{};
	};
	std::mutex                          g_tracksLock;
	std::unordered_map<Voice*, Track>   g_tracks;

	void Untrack(Voice* a_voice)
	{
		if (const auto it = g_tracks.find(a_voice); it != g_tracks.end()) {
			it->second.effect->Release();
			g_tracks.erase(it);
		}
	}

	Effect* Find(Voice* a_voice)
	{
		std::shared_lock guard(g_voicesLock);
		const auto       it = g_voices.find(a_voice);
		return it != g_voices.end() ? it->second : nullptr;
	}

	template <class T>
	T Slot(void* a_object, std::size_t a_index)
	{
		return reinterpret_cast<T>((*static_cast<void***>(a_object))[a_index]);
	}

	template <class T>
	T Patch(void* a_object, std::size_t a_index, void* a_thunk)
	{
		auto** vtable = *static_cast<void***>(a_object);
		DWORD  old{};
		VirtualProtect(&vtable[a_index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
		const auto original = vtable[a_index];
		vtable[a_index] = a_thunk;
		VirtualProtect(&vtable[a_index], sizeof(void*), old, &old);
		return reinterpret_cast<T>(original);
	}

	struct VoiceClass
	{
		void**         vtable{ nullptr };
		DestroyVoice_t destroyVoice{ nullptr };
	};
	constexpr std::uint32_t    kMaxClasses = 16;
	VoiceClass                 g_classes[kMaxClasses];
	std::atomic<std::uint32_t> g_classCount{ 0 };
	std::mutex                 g_classLock;

	const VoiceClass* ClassOf(Voice* a_voice)
	{
		auto**     vtable = *static_cast<void***>(a_voice);
		const auto count = g_classCount.load(std::memory_order_acquire);
		for (std::uint32_t i = 0; i < count; ++i) {
			if (g_classes[i].vtable == vtable) {
				return &g_classes[i];
			}
		}
		return nullptr;
	}

	DestroyVoice_t OriginalDestroyVoice(Voice* a_voice)
	{
		const auto* found = ClassOf(a_voice);
		return found ? found->destroyVoice : Slot<DestroyVoice_t>(a_voice, kDestroyVoice);
	}

	void __stdcall DestroyVoiceThunk(Voice* a_voice)
	{
		Effect* effect{ nullptr };
		{
			std::scoped_lock tracks(g_tracksLock);
			Untrack(a_voice);
		}
		{
			std::unique_lock guard(g_voicesLock);
			if (const auto it = g_voices.find(a_voice); it != g_voices.end()) {
				effect = it->second;
				g_voices.erase(it);
			}
		}
		const auto destroy = OriginalDestroyVoice(a_voice);
		destroy(a_voice);
		if (effect) {
			effect->Release();
		}
	}

	bool HookClass(Voice* a_voice)
	{
		if (ClassOf(a_voice)) {
			return true;
		}
		std::scoped_lock guard(g_classLock);
		if (ClassOf(a_voice)) {
			return true;
		}
		const auto index = g_classCount.load();
		if (index >= kMaxClasses) {
			return false;
		}
		auto& entry = g_classes[index];
		entry.vtable = *static_cast<void***>(a_voice);
		entry.destroyVoice = Patch<DestroyVoice_t>(a_voice, kDestroyVoice, reinterpret_cast<void*>(DestroyVoiceThunk));
		g_classCount.store(index + 1, std::memory_order_release);
		logger::info("Source voice class {} hooked", index + 1);
		return true;
	}

	CreateSourceVoice_t g_createSourceVoice{ nullptr };
	CoCreateInstance_t  g_coCreateInstance{ nullptr };
	void*               g_engine{ nullptr };

	HRESULT __stdcall CreateSourceVoiceThunk(void* a_engine, Voice** a_voice, const WAVEFORMATEX* a_format, std::uint32_t a_flags, float a_maxRatio, void* a_callback, const RE::XAUDIO2_VOICE_SENDS* a_sends, const RE::XAUDIO2_EFFECT_CHAIN* a_chain)
	{
		if (!a_voice || !a_format || a_format->nChannels < 1 || a_format->nChannels > kMaxChannels || (a_flags & kMusicFlag) || (a_chain && a_chain->EffectCount > 0)) {
			g_stats.others.fetch_add(1, std::memory_order_relaxed);
			return g_createSourceVoice(a_engine, a_voice, a_format, a_flags, a_maxRatio, a_callback, a_sends, a_chain);
		}
		auto*                         effect = new Effect();
		RE::XAUDIO2_EFFECT_DESCRIPTOR descriptor{ effect, TRUE, a_format->nChannels };
		RE::XAUDIO2_EFFECT_CHAIN      chain{ 1, &descriptor };
		const auto                    result = g_createSourceVoice(a_engine, a_voice, a_format, a_flags, a_maxRatio, a_callback, a_sends, &chain);
		if (FAILED(result) || !*a_voice) {
			effect->Release();
			if (g_stats.refused.fetch_add(1, std::memory_order_relaxed) < 3) {
				logger::warn("XAudio2 refused the effect (0x{:08X}): voice created without it", static_cast<std::uint32_t>(result));
			}
			return g_createSourceVoice(a_engine, a_voice, a_format, a_flags, a_maxRatio, a_callback, a_sends, a_chain);
		}
		if (!HookClass(*a_voice)) {
			logger::error("Too many source voice classes: voice recreated without the effect");
			OriginalDestroyVoice(*a_voice)(*a_voice);
			effect->Release();
			return g_createSourceVoice(a_engine, a_voice, a_format, a_flags, a_maxRatio, a_callback, a_sends, a_chain);
		}
		{
			std::unique_lock guard(g_voicesLock);
			g_voices[*a_voice] = effect;
		}
		g_stats.voices.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	bool NgaeVoiceEffects()
	{
		if (!GetModuleHandleW(L"NextGenAudioExperience.dll")) {
			return false;
		}
		std::ifstream file("Data/SKSE/Plugins/NextGenAudioExperience.ini");
		std::string   line;
		bool          on = true;
		while (std::getline(file, line)) {
			const auto equals = line.find('=');
			if (equals == std::string::npos || line.empty() || line[0] == ';' || line[0] == '#') {
				continue;
			}
			auto key = line.substr(0, equals);
			auto value = line.substr(equals + 1);
			key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c) != 0; }), key.end());
			value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; }), value.end());
			if (key == "bVoiceEffects") {
				on = value == "true" || value == "1";
			}
		}
		return on;
	}

	HRESULT __stdcall CoCreateInstanceThunk(REFCLSID a_clsid, LPUNKNOWN a_outer, DWORD a_context, REFIID a_iid, LPVOID* a_out)
	{
		const auto result = g_coCreateInstance(a_clsid, a_outer, a_context, a_iid, a_out);
		if (SUCCEEDED(result) && a_out && *a_out && !g_engine && (Same(a_clsid, kXAudio2) || Same(a_clsid, kXAudio2Debug)) && Same(a_iid, kIXAudio2)) {
			g_engine = *a_out;
			if (NgaeVoiceEffects()) {
				logger::warn("Next Gen Audio Experience is loaded with bVoiceEffects on: it already applies Steam Audio's directivity to voices, so Sound Source Directivity attaches nothing");
				return result;
			}
			g_createSourceVoice = Patch<CreateSourceVoice_t>(g_engine, kCreateSourceVoice, reinterpret_cast<void*>(CreateSourceVoiceThunk));
			logger::info("XAudio2 created: CreateSourceVoice hooked");
		}
		return result;
	}

	using GameSoundFn = void(RE::BSXAudio2GameSound*);
	REL::Relocation<GameSoundFn> g_gamePlay;

	void GamePlay(RE::BSXAudio2GameSound* a_sound)
	{
		if (auto* voice = a_sound ? static_cast<Voice*>(a_sound->sourceVoice) : nullptr) {
			std::scoped_lock tracks(g_tracksLock);
			Untrack(voice);
			if (auto* effect = Find(voice)) {
				effect->gain.store(1.0f, std::memory_order_relaxed);
				effect->directional.store(false, std::memory_order_relaxed);
				effect->restart.store(true, std::memory_order_release);
			}
		}
		g_gamePlay(a_sound);
	}

	Calculate_t g_calculate = nullptr;

	float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

	const RE::NiAVObject* HeadNode(std::uintptr_t a_sound)
	{
		auto* manager = RE::BSAudioManager::GetSingleton();
		if (!manager) {
			return nullptr;
		}
		static std::unordered_map<std::uintptr_t, std::uint32_t> ids;
		std::uint32_t id = 0;
		bool          found = false;
		if (const auto it = ids.find(a_sound); it != ids.end()) {
			if (const auto active = manager->activeSounds.find(it->second); active != manager->activeSounds.end() && reinterpret_cast<std::uintptr_t>(active->second) == a_sound) {
				id = it->second;
				found = true;
			}
		}
		if (!found) {
			for (const auto& [key, value] : manager->activeSounds) {
				if (reinterpret_cast<std::uintptr_t>(value) == a_sound) {
					id = key;
					found = true;
					break;
				}
			}
			if (!found) {
				return nullptr;
			}
			if (ids.size() > 1024) {
				ids.clear();
			}
			ids[a_sound] = id;
		}
		const auto it = manager->movingSounds.find(id);
		if (it == manager->movingSounds.end() || !it->second) {
			return nullptr;
		}
		const RE::NiAVObject* node = it->second.get();
		return std::strcmp(node->name.c_str(), "NPC Head [Head]") == 0 ? node : nullptr;
	}

	std::mutex g_cameraLock;
	Vec3       g_camera{};
	bool       g_cameraValid{ false };
	double     g_listenerGapSum{ 0.0 }, g_listenerGapMax{ 0.0 };
	std::uint64_t g_listenerGaps{ 0 };

	void Report()
	{
		const auto now = std::chrono::steady_clock::now();
		if (now - g_stats.last < 10s) {
			return;
		}
		g_stats.last = now;
		const auto updates = g_stats.updates.exchange(0);
		double     gapAverage = 0.0, gapMax = 0.0;
		{
			std::scoped_lock guard(g_cameraLock);
			gapAverage = g_listenerGaps ? g_listenerGapSum / g_listenerGaps : 0.0;
			gapMax = g_listenerGapMax;
			g_listenerGapSum = g_listenerGapMax = 0.0;
			g_listenerGaps = 0;
		}
		logger::info("Last 10 s: {} frames, {} voice updates, directivity average {:.1f} dB, lowest {:.1f} dB; Steam effect passes {}, largest gain change between passes {:.2f} dB; game listener vs camera average {:.1f} cm, largest {:.1f} cm; voices with the effect {} (others {}, refused {})",
			g_stats.frames.exchange(0), updates, updates ? 20.0 * std::log10((std::max)(g_stats.sum / updates, 1e-6)) : 0.0, 20.0f * std::log10((std::max)(g_stats.lowest.exchange(1.0f), 1e-6f)),
			g_stats.applied.exchange(0), g_stats.largestStep.exchange(0.0f), gapAverage * 1.42875, gapMax * 1.42875, g_stats.voices.load(), g_stats.others.load(), g_stats.refused.load());
		g_stats.sum = 0.0;
		if (g_stats.seen.size() > 4096) {
			g_stats.seen.clear();
		}
	}

	void Frame()
	{
		const auto& settings = Settings::GetSingleton();
		auto*       camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->cameraRoot) {
			return;
		}
		const auto& c = camera->cameraRoot->world.translate;
		const Vec3  hear{ c.x, c.z, c.y };
		{
			std::scoped_lock guard(g_cameraLock);
			g_camera = hear;
			g_cameraValid = true;
		}
		const auto now = std::chrono::steady_clock::now();
		std::scoped_lock guard(g_tracksLock);
		for (auto it = g_tracks.begin(); it != g_tracks.end();) {
			auto& track = it->second;
			if (now - track.seen > 500ms || !track.node) {
				track.effect->Release();
				it = g_tracks.erase(it);
				continue;
			}
			const auto& m = track.node->world.rotate.entry;
			const auto& t = track.node->world.translate;
			const Vec3  face{ m[0][1], m[2][1], m[1][1] };
			const Vec3  mouth{ t.x, t.z, t.y };
			float       value = 1.0f;
			if (settings.directivity) {
				const float length = std::sqrt(Dot(face, face));
				IPLCoordinateSpace3 source{};
				source.ahead = length > 0.0f ? IPLVector3{ face.x / length, face.y / length, face.z / length } : IPLVector3{ 0.0f, 0.0f, 1.0f };
				source.up = { 0.0f, 1.0f, 0.0f };
				source.right = { 1.0f, 0.0f, 0.0f };
				source.origin = { mouth.x, mouth.y, mouth.z };
				IPLDirectivity model{ settings.dipoleWeight, settings.dipolePower, nullptr, nullptr };
				value = g_phonon.directivity(g_phonon.context, source, IPLVector3{ hear.x, hear.y, hear.z }, &model);
			}
			track.effect->gain.store(value, std::memory_order_relaxed);
			track.effect->directional.store(true, std::memory_order_release);
			if (settings.log) {
				g_stats.updates.fetch_add(1, std::memory_order_relaxed);
				g_stats.sum += value;
				if (value < g_stats.lowest.load(std::memory_order_relaxed)) {
					g_stats.lowest.store(value, std::memory_order_relaxed);
				}
				if (g_stats.examples < 50 && g_stats.seen.insert(reinterpret_cast<std::uintptr_t>(it->first)).second) {
					++g_stats.examples;
					const Vec3  d{ hear.x - mouth.x, hear.y - mouth.y, hear.z - mouth.z };
					const float distance = std::sqrt(Dot(d, d));
					const float angle = distance > 1e-6f ? std::acos(std::clamp(Dot(d, face) / (distance * std::sqrt(Dot(face, face))), -1.0f, 1.0f)) * 57.29578f : 0.0f;
					logger::info("Voice: {:.1f} m away, listener {:.0f} deg off the speaker's facing, directivity {:.1f} dB", distance * 0.0142875f, angle, 20.0f * std::log10((std::max)(value, 1e-6f)));
				}
			}
			++it;
		}
		if (settings.log) {
			g_stats.frames.fetch_add(1, std::memory_order_relaxed);
			Report();
		}
	}

	REL::Relocation<void (*)()> g_frameStub;

	void FrameHook()
	{
		g_frameStub();
		Frame();
	}

	bool InstallFrameHook()
	{
		const auto update = REL::Relocation<std::uintptr_t>(RELOCATION_ID(35565, 36564)).address();
		const auto stub = REL::Relocation<std::uintptr_t>(RELOCATION_ID(76765, 78641)).address();
		std::uintptr_t site = 0;
		int            hits = 0;
		for (std::uintptr_t at = update; at < update + 0x1000; ++at) {
			if (*reinterpret_cast<const std::uint8_t*>(at) == 0xE8 && at + 5 + *reinterpret_cast<const std::int32_t*>(at + 1) == stub) {
				site = at;
				++hits;
			}
		}
		if (hits != 1) {
			logger::error("Per-frame hook: {} calls to the frame stub found in Main::Update (expected 1), directivity off", hits);
			return false;
		}
		SKSE::AllocTrampoline(14);
		g_frameStub = SKSE::GetTrampoline().write_call<5>(site, FrameHook);
		logger::info("Per-frame hook at Main::Update +{:#x}", site - update);
		return true;
	}

	void CalculateThunk(const void* a_instance, const Listener* a_listener, const Emitter* a_emitter, std::uint32_t a_flags, DSPSettings* a_dsp)
	{
		g_calculate(a_instance, a_listener, a_emitter, a_flags, a_dsp);
		if (!a_listener || !a_emitter || !a_dsp) {
			return;
		}
		const auto sound = reinterpret_cast<std::uintptr_t>(a_emitter) - kEmitter;
		if (reinterpret_cast<std::uintptr_t>(a_dsp) != sound + kDSP) {
			return;
		}
		auto* const voice = *reinterpret_cast<Voice**>(sound + kVoice);
		Effect* const effect = voice ? Find(voice) : nullptr;
		const RE::NiAVObject* node = effect ? HeadNode(sound) : nullptr;
		if (!node) {
			return;
		}
		{
			std::scoped_lock guard(g_tracksLock);
			auto& track = g_tracks[voice];
			if (track.effect != effect) {
				if (track.effect) {
					track.effect->Release();
				}
				effect->AddRef();
				track.effect = effect;
			}
			if (track.node.get() != node) {
				track.node = RE::NiPointer<RE::NiAVObject>(const_cast<RE::NiAVObject*>(node));
			}
			track.seen = std::chrono::steady_clock::now();
		}
		if (Settings::GetSingleton().log) {
			std::scoped_lock guard(g_cameraLock);
			if (g_cameraValid) {
				const Vec3   d{ a_listener->position.x - g_camera.x, a_listener->position.y - g_camera.y, a_listener->position.z - g_camera.z };
				const double gap = std::sqrt(double(Dot(d, d)));
				g_listenerGapSum += gap;
				g_listenerGapMax = (std::max)(g_listenerGapMax, gap);
				++g_listenerGaps;
			}
		}
	}
}

void Directivity::Install()
{
	if (!LoadPhonon()) {
		return;
	}
	const auto calculate = SKSE::PatchIAT(CalculateThunk, "X3DAudio1_7.dll", "X3DAudioCalculate");
	if (!calculate) {
		logger::error("X3DAudioCalculate import not found, nothing installed");
		return;
	}
	g_calculate = reinterpret_cast<Calculate_t>(calculate);
	const auto create = SKSE::PatchIAT(CoCreateInstanceThunk, "ole32.dll", "CoCreateInstance");
	if (!create) {
		logger::error("CoCreateInstance import not found: no voices will get the effect");
		return;
	}
	g_coCreateInstance = reinterpret_cast<CoCreateInstance_t>(create);
	REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSXAudio2GameSound[0] };
	g_gamePlay = vtable.write_vfunc(0x15, GamePlay);
	InstallFrameHook();
	logger::info("Installed: X3DAudioCalculate and CoCreateInstance imports, game sound PlayImpl; dipole weight {}, power {}", Settings::GetSingleton().dipoleWeight, Settings::GetSingleton().dipolePower);
}
