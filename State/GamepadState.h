#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <Limelight.h>
#include "Utils.hpp"

enum class ComboState {
	None,
	ViewWaiting,
	MenuWaiting,
	ComboActive
};

struct GamepadComboState {
	bool viewPressed = false;
	bool menuPressed = false;
	int64_t startTime = 0;
	ComboState comboState = ComboState::None;
};

struct ComboResult {
	Windows::Gaming::Input::GamepadReading currentReading; // untouched reading
	Windows::Gaming::Input::GamepadReading maskedReading;  // reading with pending combo buttons masked out
	bool comboTriggered;                                   // True when combo completes
};

struct GamepadState {
	Windows::Gaming::Input::Gamepad ^ controller;
	uint32_t localId;                           // index into Gamepad::Gamepads vector, used during add/remove
	uint8_t hostId;                             // index this host is mapped to on the host, used for all commands
	int64_t lastRefreshedQpc;                   // timestamp of last refresh
	bool didSendArrival;                        // do we need to send LiSendControllerArrivalEvent?
	int64_t lastArrivalAttemptQpc;
	std::atomic<bool> isGuideButtonDown{false}; // are we currently holding down the (virtual) Guide button?
	// Battery work is due immediately on connection, then at most once every two minutes.
	int64_t nextBatteryPollQpc;
	bool shouldUpdateBattery;
	uint8_t batteryState;
	uint8_t batteryPercentage;
	uint8_t lastBatteryState;
	uint8_t lastBatteryPercentage;
	bool didSendBattery;
	bool batteryReportingUnsupported;

	Windows::Gaming::Input::GamepadReading reading;
	Windows::Gaming::Input::GamepadReading previousReading;
	bool previousGuideButtonDown;
	GamepadComboState combo;

	short ltX, ltY, rtX, rtY;   // after a call to normalizeAxes() these are
	unsigned char lTrig, rTrig; // populated with values expected by the protocol

	// Mouse mode integrator state. The pointer and scroll velocities are computed in
	// units/second and integrated against real elapsed time, so the sub-unit remainder
	// has to survive across polls -- see UpdatePointer() in moonlight_xbox_dxMain.cpp.
	double mouseAccumX = 0.0, mouseAccumY = 0.0;   // sub-pixel remainder not yet sent to the host
	double mouseSmoothMag = 0.0;                   // low-passed stick magnitude
	int64_t mouseLastQpc = 0;                      // timestamp of the last pointer integration
	double scrollAccumV = 0.0, scrollAccumH = 0.0; // sub-unit scroll remainder
	int64_t scrollLastQpc = 0;                     // timestamp of the last scroll integration
	int64_t scrollLastSendQpc = 0;                 // timestamp of the last scroll event sent

	static inline Windows::Gaming::Input::GamepadReading EmptyReading() {
		return Windows::Gaming::Input::GamepadReading{};
	}

	static inline bool isPressed(Windows::Gaming::Input::GamepadButtons buttons, Windows::Gaming::Input::GamepadButtons b) {
		return (buttons & b) == b;
	}

	static inline Windows::Gaming::Input::GamepadButtons clearButtons(Windows::Gaming::Input::GamepadButtons buttons, Windows::Gaming::Input::GamepadButtons mask) {
		return static_cast<Windows::Gaming::Input::GamepadButtons>(
		    static_cast<uint32_t>(buttons) & ~static_cast<uint32_t>(mask));
	}

	static inline Windows::Gaming::Input::GamepadButtons setButtons(Windows::Gaming::Input::GamepadButtons buttons, Windows::Gaming::Input::GamepadButtons mask) {
		return static_cast<Windows::Gaming::Input::GamepadButtons>(
		    static_cast<uint32_t>(buttons) | static_cast<uint32_t>(mask));
	}

	void Reset() {
		controller = nullptr;
		localId = hostId = 0;
		lastRefreshedQpc = 0;
		didSendArrival = false;
		lastArrivalAttemptQpc = 0;
		isGuideButtonDown.store(false);
		nextBatteryPollQpc = 0;
		shouldUpdateBattery = false;
		batteryState = lastBatteryState = LI_BATTERY_STATE_UNKNOWN;
		batteryPercentage = lastBatteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
		didSendBattery = false;
		batteryReportingUnsupported = false;
		previousGuideButtonDown = false;
		reading = EmptyReading();
		previousReading = EmptyReading();
		ltX = ltY = rtX = rtY = 0;
		lTrig = rTrig = 0;
		ResetPointer();
		combo.comboState = ComboState::None;
		combo.viewPressed = false;
		combo.menuPressed = false;
		combo.startTime = 0;
	}

	// Called only when due, after sending input. Advance the deadline before reading so
	// exceptions and failed sends also wait a full interval before retrying.
	bool UpdateBattery() {
		if (!shouldUpdateBattery || controller == nullptr || batteryReportingUnsupported) {
			return false;
		}
		shouldUpdateBattery = false;
		nextBatteryPollQpc = QpcNow() + MsToQpc(kBatteryPollIntervalMs);

		try {
			ReadBattery();
		} catch (Platform::Exception ^ exception) {
			moonlight_xbox_dx::Utils::Logf("UpdateBattery: failed to read Gamepad #%d battery: 0x%08X\n", localId, static_cast<unsigned>(exception->HResult));
			return false;
		}

		return !didSendBattery || batteryState != lastBatteryState || batteryPercentage != lastBatteryPercentage;
	}

	void OnBatterySent(int rc) {
		if (rc == LI_ERR_UNSUPPORTED) {
			batteryReportingUnsupported = true;
			moonlight_xbox_dx::Utils::Logf("SendGamepadBattery: host does not support battery reporting for Gamepad #%d\n", localId);
			return;
		}
		if (rc != 0) {
			moonlight_xbox_dx::Utils::Logf("SendGamepadBattery: failed to send Gamepad #%d battery: %d\n", localId, rc);
			return;
		}

		lastBatteryState = batteryState;
		lastBatteryPercentage = batteryPercentage;
		didSendBattery = true;
		moonlight_xbox_dx::Utils::Logf("SendGamepadBattery: sent Gamepad #%d state %d at %d%%\n", localId, batteryState, batteryPercentage);
	}

private:
	static constexpr int64_t kBatteryPollIntervalMs = 120 * 1000;

	void ReadBattery() {
		using namespace Windows::System::Power;

		batteryState = LI_BATTERY_STATE_UNKNOWN;
		batteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
		auto report = controller->TryGetBatteryReport();
		if (report == nullptr) {
			return;
		}

		auto remaining = report->RemainingCapacityInMilliwattHours;
		auto full = report->FullChargeCapacityInMilliwattHours;
		if (remaining != nullptr && full != nullptr && full->Value > 0) {
			const double capacity = static_cast<double>(remaining->Value) / full->Value;
			batteryPercentage = static_cast<uint8_t>(std::clamp(std::lround(capacity * 100.0), 0L, 100L));
		}

		switch (report->Status) {
		case BatteryStatus::NotPresent:
			batteryState = LI_BATTERY_STATE_NOT_PRESENT;
			batteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
			break;
		case BatteryStatus::Discharging:
			batteryState = LI_BATTERY_STATE_DISCHARGING;
			break;
		case BatteryStatus::Charging:
			batteryState = LI_BATTERY_STATE_CHARGING;
			break;
		case BatteryStatus::Idle:
			batteryState = batteryPercentage == 100 ? LI_BATTERY_STATE_FULL : LI_BATTERY_STATE_NOT_CHARGING;
			break;
		}
	}

public:
	// Drop any pending sub-unit motion and force the next poll to re-seed its timestamps.
	// Entering mouse mode doesn't need to call this: the first poll after a gap longer than
	// kMaxDtSec is discarded and re-seeds itself, so a stale dt can't fling the cursor.
	void ResetPointer() {
		mouseAccumX = mouseAccumY = 0.0;
		mouseSmoothMag = 0.0;
		mouseLastQpc = 0;
		scrollAccumV = scrollAccumH = 0.0;
		scrollLastQpc = 0;
		scrollLastSendQpc = 0;
	}

	void SetGuideButtonDown(bool isDown) {
		isGuideButtonDown.store(isDown);
	}

	bool GetGuideButtonDown() {
		return isGuideButtonDown.load();
	}

	ComboResult GetComboResult(int comboTimeoutMs, int64_t now) {
		using namespace Windows::Gaming::Input;

		shouldUpdateBattery = false;
		ComboResult result = ComboResult{EmptyReading(), EmptyReading(), false};
		if (controller == nullptr) {
			return result;
		}

		auto gamepads = Gamepad::Gamepads;
		if (localId >= gamepads->Size) {
			return result;
		}

		Gamepad ^ gamepad = gamepads->GetAt(localId);
		if (gamepad == nullptr) {
			return result;
		}

		// Reuse the input loop clock: no clock query, interval conversion, or battery API
		// call on the usual 500 Hz path, even when the controller reading is unchanged.
		shouldUpdateBattery = didSendArrival && !batteryReportingUnsupported && now >= nextBatteryPollQpc;

		result.currentReading = gamepad->GetCurrentReading();
		auto buttons = result.currentReading.Buttons;

		const bool viewCurrentlyPressed = isPressed(buttons, GamepadButtons::View);
		const bool menuCurrentlyPressed = isPressed(buttons, GamepadButtons::Menu);

		// Start with unmasked buttons
		GamepadButtons maskedButtons = buttons;

		switch (combo.comboState) {
		case ComboState::None:
			// Handle simultaneous press in the same poll
			if (viewCurrentlyPressed && menuCurrentlyPressed && !combo.viewPressed && !combo.menuPressed) {
				combo.comboState = ComboState::ComboActive;
				result.comboTriggered = true;
				maskedButtons = clearButtons(buttons, GamepadButtons::View | GamepadButtons::Menu);
				// Check if either button is newly pressed
			} else if (viewCurrentlyPressed && !combo.viewPressed) {
				combo.comboState = ComboState::ViewWaiting;
				combo.startTime = now;
				maskedButtons = clearButtons(buttons, GamepadButtons::View);
			} else if (menuCurrentlyPressed && !combo.menuPressed) {
				combo.comboState = ComboState::MenuWaiting;
				combo.startTime = now;
				maskedButtons = clearButtons(buttons, GamepadButtons::Menu);
			}
			break;

		case ComboState::ViewWaiting:
			// Check timeout
			if (QpcToMs(now - combo.startTime) > comboTimeoutMs) {
				combo.comboState = ComboState::None;
				maskedButtons = setButtons(buttons, GamepadButtons::View);
				break;
			}

			// Check if View was released before combo completed
			if (!viewCurrentlyPressed) {
				combo.comboState = ComboState::None;
				maskedButtons = setButtons(buttons, GamepadButtons::View);
				break;
			}

			// Mask View while waiting
			maskedButtons = clearButtons(buttons, GamepadButtons::View);

			// Check if Menu is now pressed (combo complete)
			if (menuCurrentlyPressed && !combo.menuPressed) {
				combo.comboState = ComboState::ComboActive;
				result.comboTriggered = true;
				maskedButtons = clearButtons(buttons, GamepadButtons::View | GamepadButtons::Menu);
			}
			break;

		case ComboState::MenuWaiting:
			if (QpcToMs(now - combo.startTime) > comboTimeoutMs) {
				combo.comboState = ComboState::None;
				maskedButtons = setButtons(buttons, GamepadButtons::Menu);
				break;
			}

			if (!menuCurrentlyPressed) {
				combo.comboState = ComboState::None;
				maskedButtons = setButtons(buttons, GamepadButtons::Menu);
				break;
			}

			maskedButtons = clearButtons(buttons, GamepadButtons::Menu);

			if (viewCurrentlyPressed && !combo.viewPressed) {
				combo.comboState = ComboState::ComboActive;
				result.comboTriggered = true;
				maskedButtons = clearButtons(buttons, GamepadButtons::View | GamepadButtons::Menu);
			}
			break;

		case ComboState::ComboActive:
			// Remain in combo state while both are held
			if (viewCurrentlyPressed && menuCurrentlyPressed) {
				// Continue masking both buttons
				maskedButtons = clearButtons(buttons, GamepadButtons::View | GamepadButtons::Menu);
			} else {
				// One or both released, reset state
				combo.comboState = ComboState::None;
			}
			break;
		}

		combo.viewPressed = viewCurrentlyPressed;
		combo.menuPressed = menuCurrentlyPressed;
		result.maskedReading = result.currentReading;
		result.maskedReading.Buttons = maskedButtons;

		return result;
	}

	static inline short ClampAxis(float v) {
		if (!std::isfinite(v)) return 0;
		v = std::clamp(v, -1.0f, 1.0f);

		// Map [-1.0,1.0] -> [-32768,32767]
		int s = (int)std::lrintf(v * 32768.0f);
		s = std::clamp(s, -32768, 32767);
		return (short)s;
	}

	static inline unsigned char ClampTrigger(float v) {
		if (!std::isfinite(v)) return 0;
		v = std::clamp(v, 0.0f, 1.0f);

		int t = (int)std::lrintf(v * 255.0f);
		t = std::clamp(t, 0, 255);
		return (unsigned char)t;
	}

	void normalizeAxes() {
		ltX = ClampAxis((float)reading.LeftThumbstickX);
		ltY = ClampAxis((float)reading.LeftThumbstickY);
		rtX = ClampAxis((float)reading.RightThumbstickX);
		rtY = ClampAxis((float)reading.RightThumbstickY);

		lTrig = ClampTrigger((float)reading.LeftTrigger);
		rTrig = ClampTrigger((float)reading.RightTrigger);
	}

	bool hasGamepadReadingChanged() {
		auto p = previousReading;

		if (reading.Buttons != p.Buttons) {
			return true;
		}

		short pltX = ClampAxis((float)p.LeftThumbstickX);
		short pltY = ClampAxis((float)p.LeftThumbstickY);
		short prtX = ClampAxis((float)p.RightThumbstickX);
		short prtY = ClampAxis((float)p.RightThumbstickY);
		if (ltX != pltX || ltY != pltY || rtX != prtX || rtY != prtY) {
			return true;
		}

		unsigned char plTrig = ClampTrigger((float)p.LeftTrigger);
		unsigned char prTrig = ClampTrigger((float)p.RightTrigger);
		if (lTrig != plTrig || rTrig != prTrig) {
			return true;
		}

		bool guideButtonDown = isGuideButtonDown.load();
		if (guideButtonDown != previousGuideButtonDown) {
			previousGuideButtonDown = guideButtonDown;
			return true;
		}

		return false;
	}

	// Debug helpers
	static inline const struct {
		Windows::Gaming::Input::GamepadButtons bit;
		const char *name;
	} kGamepadButtonNames[] = {
	    {Windows::Gaming::Input::GamepadButtons::Menu, "Menu"},
	    {Windows::Gaming::Input::GamepadButtons::View, "View"},
	    {Windows::Gaming::Input::GamepadButtons::A, "A"},
	    {Windows::Gaming::Input::GamepadButtons::B, "B"},
	    {Windows::Gaming::Input::GamepadButtons::X, "X"},
	    {Windows::Gaming::Input::GamepadButtons::Y, "Y"},
	    {Windows::Gaming::Input::GamepadButtons::DPadUp, "Up"},
	    {Windows::Gaming::Input::GamepadButtons::DPadDown, "Down"},
	    {Windows::Gaming::Input::GamepadButtons::DPadLeft, "Left"},
	    {Windows::Gaming::Input::GamepadButtons::DPadRight, "Right"},
	    {Windows::Gaming::Input::GamepadButtons::LeftShoulder, "LB"},
	    {Windows::Gaming::Input::GamepadButtons::RightShoulder, "RB"},
	    {Windows::Gaming::Input::GamepadButtons::LeftThumbstick, "L3"},
	    {Windows::Gaming::Input::GamepadButtons::RightThumbstick, "R3"},
	    {Windows::Gaming::Input::GamepadButtons::Paddle1, "P1"},
	    {Windows::Gaming::Input::GamepadButtons::Paddle2, "P2"},
	    {Windows::Gaming::Input::GamepadButtons::Paddle3, "P3"},
	    {Windows::Gaming::Input::GamepadButtons::Paddle4, "P4"},
	};

	static void DumpButtons(Windows::Gaming::Input::GamepadButtons buttons, char *out, size_t outSize) {
		size_t pos = 0;
		bool first = true;

		for (const auto &b : kGamepadButtonNames) {
			if ((static_cast<uint32_t>(buttons) & static_cast<uint32_t>(b.bit)) == static_cast<uint32_t>(b.bit)) {
				int written = std::snprintf(
				    out + pos,
				    outSize - pos,
				    "%s%s",
				    first ? "" : "|",
				    b.name);
				if (written <= 0 || static_cast<size_t>(written) >= outSize - pos)
					break;

				pos += written;
				first = false;
			}
		}

		if (first) {
			std::snprintf(out, outSize, "None");
		}
	}

	void DumpState() {
		char buttons[128];
		DumpButtons(reading.Buttons, buttons, sizeof(buttons));
		moonlight_xbox_dx::Utils::Logf(
		    "GamepadState[localId: %d, hostId: %d] buttons: %s %s axes: %d %d, %d %d, triggers: %d %d, combo{ state: %d, viewPressed: %d, menuPressed: %d, startTime: %d }, battery{ state: %d, percentage: %d }\n",
		    localId, hostId,
		    buttons,
		    isGuideButtonDown.load() ? "Guide" : "",
		    ltX, ltY, rtX, rtY,
		    lTrig, rTrig,
		    combo.comboState, combo.viewPressed, combo.menuPressed, combo.startTime,
		    batteryState, batteryPercentage);
	}
};
