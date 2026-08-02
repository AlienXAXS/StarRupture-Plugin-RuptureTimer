#include "debug/object_dumper.h"

#if !defined(RUPTURETIMER_DEBUG_TOOLS)

namespace RuptureTimer
{
	void ObjectDumper::RequestDump() {}
	void ObjectDumper::PumpPendingDump() {}
	bool ObjectDumper::IsDumpPending() { return false; }
}

#else

#include "plugin_helpers.h"

#include "CoreUObject_classes.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace RuptureTimer
{
	namespace
	{
		std::atomic<bool> g_dumpPending{ false };

		// Any object whose class OR object name contains one of these is dumped.
		const char* kNameFilters[] = { "Wave", "Rupture", "Enviro" };

		// GObjects is mostly reflection metadata -- every UClass, UFunction and
		// UScriptStruct is itself an object in there. Matching on name alone
		// pulls in the UClass named "SoundWave" rather than anything that holds
		// wave state, and inspecting one just describes UClass itself. Only
		// instances are interesting here.
		const char* kSkipClasses[] = {
			"Class", "ScriptStruct", "Function", "DelegateFunction",
			"SparseDelegateFunction", "Package", "Enum", "Interface",
			"UserDefinedEnum", "UserDefinedStruct", "VerseClass",
			"BlueprintGeneratedClass", "WidgetBlueprintGeneratedClass",
			"AnimBlueprintGeneratedClass", "DynamicClass",
		};

		// Audio, VFX and the Rupture's own cosmetic components own a lot of
		// legitimately wave-named types that hold no timing state. Left in, they
		// swamp the object budget -- a first run spent all 250 slots on sound
		// players and material-map components and never reached the subsystems.
		const char* kSkipNameFragments[] = {
			// Audio
			"SoundWave", "Waveform", "WaveTable", "DialogueWave", "WaveInstance",
			"SoundNode", "SoundCue", "AudioComponent",
			// VFX / data assets
			"Niagara", "MaterialFunction", "Curve", "PCGNode", "BlackboardKeyType",
			// Rupture visuals only -- geometry and material driving, no schedule
			"CrEnviroWaveMaterialMap", "CrEnviroWaveBiomesHISM",
		};

		// --- Reflection walking ---------------------------------------------
		// The SDK names UStruct::SuperStruct and ::ChildProperties, but stops at
		// `class FField*` -- it never describes FField itself. These offsets are
		// from the IDA dump (_Types/FField.txt, _Types/FProperty.txt) and are the
		// only way to enumerate properties: IPluginObjectProperties can resolve a
		// property by name but cannot list them, which is no use when the whole
		// point is discovering names we do not know.
		constexpr int kOff_FField_ClassPrivate  = 0x08;  // FFieldClass*
		constexpr int kOff_FField_Next          = 0x18;  // FField*
		constexpr int kOff_FField_NamePrivate   = 0x20;  // FName
		constexpr int kOff_FFieldClass_Name     = 0x00;  // FName
		constexpr int kOff_FProperty_ElementSize = 0x34; // int32
		constexpr int kOff_FProperty_Offset     = 0x44;  // int32

		// Keeps the log readable, and bounds the cost if a filter is too broad.
		constexpr int kMaxObjects           = 600;
		constexpr int kMaxPropertiesPerType = 80;

		// ~400 bytes per entry, so this bounds the transient allocation at about
		// 160 MB in the worst case. One-shot, freed as soon as the dump ends.
		constexpr int kWalkCapacityCap      = 400000;

		// The walk depends on the compiler laying UStruct out exactly as the dump
		// describes. Proven here rather than assumed -- if a re-dump shifts these,
		// the build fails instead of silently reporting "no properties".
		static_assert(offsetof(SDK::UStruct, SuperStruct)     == 0x40, "UStruct::SuperStruct moved");
		static_assert(offsetof(SDK::UStruct, ChildProperties) == 0x50, "UStruct::ChildProperties moved");

		template <typename T>
		T ReadAt(const void* base, int offset)
		{
			T v{};
			std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(T));
			return v;
		}

		std::string FNameAt(const void* base, int offset)
		{
			const auto* name = reinterpret_cast<const SDK::FName*>(
				static_cast<const uint8_t*>(base) + offset);
			return name->ToString();
		}

		bool MatchesFilter(const char* className, const char* objectName)
		{
			if (!className) return false;

			for (const char* skip : kSkipClasses)
				if (strcmp(className, skip) == 0) return false;

			for (const char* skip : kSkipNameFragments)
			{
				if (strstr(className, skip)) return false;
				if (objectName && strstr(objectName, skip)) return false;
			}

			for (const char* needle : kNameFilters)
			{
				if (strstr(className, needle)) return true;
				if (objectName && strstr(objectName, needle)) return true;
			}
			return false;
		}

		// Renders the property's current value when it is a kind worth reading.
		// Anything else just reports its type, which is still enough to spot a
		// candidate worth chasing.
		void FormatValue(const char* typeName, const void* object, int offset,
		                 char* out, size_t outLen)
		{
			out[0] = '\0';
			if (!object || offset < 0) return;

			const void* field = static_cast<const uint8_t*>(object) + offset;

			if (strcmp(typeName, "FloatProperty") == 0)
				snprintf(out, outLen, " = %.3f", ReadAt<float>(field, 0));
			else if (strcmp(typeName, "DoubleProperty") == 0)
				snprintf(out, outLen, " = %.3f", ReadAt<double>(field, 0));
			else if (strcmp(typeName, "IntProperty") == 0)
				snprintf(out, outLen, " = %d", ReadAt<int32_t>(field, 0));
			else if (strcmp(typeName, "Int64Property") == 0)
				snprintf(out, outLen, " = %lld", (long long)ReadAt<int64_t>(field, 0));
			else if (strcmp(typeName, "BoolProperty") == 0)
				snprintf(out, outLen, " = %s", ReadAt<uint8_t>(field, 0) ? "true" : "false");
			else if (strcmp(typeName, "ByteProperty") == 0 || strcmp(typeName, "EnumProperty") == 0)
				snprintf(out, outLen, " = %u", (unsigned)ReadAt<uint8_t>(field, 0));
			else if (strcmp(typeName, "ObjectProperty") == 0)
				snprintf(out, outLen, " = %p", ReadAt<void*>(field, 0));
		}

		void DumpProperties(SDK::UStruct* type, const void* object)
		{
			int printed = 0;

			// Walk the inheritance chain so inherited properties show up too --
			// a Blueprint's own variables are usually on the leaf class, but the
			// timing state could sit on a native parent.
			for (SDK::UStruct* s = type; s && printed < kMaxPropertiesPerType; s = s->SuperStruct)
			{
				const std::string typeName = s->GetName();

				for (void* field = s->ChildProperties;
				     field && printed < kMaxPropertiesPerType;
				     field = ReadAt<void*>(field, kOff_FField_Next))
				{
					const std::string propName = FNameAt(field, kOff_FField_NamePrivate);

					std::string kind = "?";
					if (void* fieldClass = ReadAt<void*>(field, kOff_FField_ClassPrivate))
						kind = FNameAt(fieldClass, kOff_FFieldClass_Name);

					const int32_t offset = ReadAt<int32_t>(field, kOff_FProperty_Offset);
					const int32_t size   = ReadAt<int32_t>(field, kOff_FProperty_ElementSize);

					char value[96];
					FormatValue(kind.c_str(), object, offset, value, sizeof(value));

					LOG_INFO("      +0x%04X (%2d) %-22s %s%s   [%s]",
						offset, size, kind.c_str(), propName.c_str(), value, typeName.c_str());
					++printed;
				}
			}

			if (printed == 0)
			{
				// Distinguishes "the chain was not read" from "this type really
				// has none" -- a null SuperStruct on a leaf class means the
				// former, which is a bug rather than an answer.
				LOG_INFO("      (no reflected properties) [type=%p ChildProperties=%p SuperStruct=%p]",
					(void*)type,
					type ? (void*)type->ChildProperties : nullptr,
					type ? (void*)type->SuperStruct     : nullptr);
			}
			else if (printed >= kMaxPropertiesPerType)
				LOG_INFO("      ... truncated at %d properties", kMaxPropertiesPerType);
		}

		void RunDump()
		{
			auto* hooks = GetHooks();
			if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
			{
				LOG_WARN("ObjectDumper: object walker not ready");
				return;
			}

			LOG_INFO("===== RuptureTimer object dump =====");

			// Probe first. GObjects holds a few hundred thousand entries and the
			// walk fills in GObjects order, so a fixed buffer scans only the
			// engine reflection objects at the front and never reaches gameplay
			// instances at all -- which is exactly what a fixed 16384 did.
			PluginObjectInfo probe{};
			const int total = hooks->ObjectWalker->WalkAllObjectsInto(
				PluginObjectLookup_InstanceOnly, &probe, 1);

			if (total <= 0)
			{
				LOG_WARN("ObjectDumper: walk returned %d objects", total);
				return;
			}

			const int capacity = total < kWalkCapacityCap ? total : kWalkCapacityCap;
			const size_t bytes = static_cast<size_t>(capacity) * sizeof(PluginObjectInfo);
			LOG_INFO("  %d objects in GObjects; scanning %d (%.1f MB buffer)",
				total, capacity, bytes / (1024.0 * 1024.0));

			std::vector<PluginObjectInfo> objects(capacity);
			const int filled  = hooks->ObjectWalker->WalkAllObjectsInto(
				PluginObjectLookup_InstanceOnly, objects.data(), capacity);
			const int scanned = filled < capacity ? filled : capacity;

			if (total > kWalkCapacityCap)
				LOG_WARN("ObjectDumper: capped at %d of %d objects", kWalkCapacityCap, total);

			int matched = 0;
			for (int i = 0; i < scanned && matched < kMaxObjects; ++i)
			{
				const PluginObjectInfo& info = objects[i];
				if (!MatchesFilter(info.className, info.objectName)) continue;
				++matched;

				auto* obj = static_cast<SDK::UObject*>(info.object);
				if (!obj || !obj->Class) continue;

				LOG_INFO("  [%d] %s  '%s'  @ %p", matched, info.className, info.objectName, info.object);
				DumpProperties(obj->Class, obj);
			}

			if (matched >= kMaxObjects)
			{
				// Loud, because a silent cap looks exactly like "the object is
				// not there" -- which is how the first run was misread.
				LOG_WARN("ObjectDumper: hit the %d object cap; results are INCOMPLETE and "
				         "later objects were never reached. Tighten kSkipNameFragments.",
				         kMaxObjects);
			}

			LOG_INFO("===== dump complete: %d of %d objects matched =====", matched, scanned);
		}
	}

	void ObjectDumper::RequestDump()     { g_dumpPending.store(true, std::memory_order_release); }
	bool ObjectDumper::IsDumpPending()   { return g_dumpPending.load(std::memory_order_acquire); }

	void ObjectDumper::PumpPendingDump()
	{
		if (!g_dumpPending.exchange(false, std::memory_order_acq_rel)) return;
		RunDump();
	}
}

#endif // RUPTURETIMER_DEBUG_TOOLS
