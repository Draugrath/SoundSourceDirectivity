#pragma once

#include "PCH.h"

class Settings
{
public:
	static Settings& GetSingleton()
	{
		static Settings instance;
		return instance;
	}

	static constexpr const char* kPath = "Data/SKSE/Plugins/SoundSourceDirectivity.ini";

	void Load()
	{
		std::ifstream file(kPath);
		std::string   line;
		while (std::getline(file, line)) {
			const auto equals = line.find('=');
			if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[' || equals == std::string::npos) {
				continue;
			}
			const auto key = Trim(line.substr(0, equals));
			const auto value = Trim(line.substr(equals + 1));
			if (key == "bEnabled") {
				enabled = Flag(value);
			} else if (key == "bDirectivity") {
				directivity = Flag(value);
			} else if (key == "fDipoleWeight") {
				Number(value, dipoleWeight, 0.0f, 1.0f);
			} else if (key == "fDipolePower") {
				Number(value, dipolePower, 0.0f, 4.0f);
			} else if (key == "bLog") {
				log = Flag(value);
			}
		}
		logger::info("Settings: enabled {}, directivity {}, dipole weight {}, power {}, log {}", enabled, directivity, dipoleWeight, dipolePower, log);
	}

	bool  enabled{ true };
	bool  directivity{ true };
	float dipoleWeight{ 0.275f };
	float dipolePower{ 1.0f };
	bool  log{ false };

private:
	static bool Flag(const std::string& a_value) { return a_value == "true" || a_value == "1"; }

	static void Number(const std::string& a_value, float& a_out, float a_min, float a_max)
	{
		try {
			a_out = std::clamp(std::stof(a_value), a_min, a_max);
		} catch (...) {
		}
	}

	static std::string Trim(std::string a_text)
	{
		const auto first = a_text.find_first_not_of(" \t\r");
		if (first == std::string::npos) {
			return {};
		}
		const auto last = a_text.find_last_not_of(" \t\r");
		return a_text.substr(first, last - first + 1);
	}
};
