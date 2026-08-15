KNX_DPT_CATALOG_ENTRY(Bool, Boolean, 0, Boolean, bool, Dpt1, "DPT-1", "1-bit value")
KNX_DPT_CATALOG_ENTRY(Switch, Boolean, 1, Boolean, bool, Dpt1, "DPT-1.001", "Switch")
KNX_DPT_CATALOG_ENTRY(Enable, Boolean, 3, Boolean, bool, Dpt1, "DPT-1.003", "Enable")
KNX_DPT_CATALOG_ENTRY(Ramp, Boolean, 4, Boolean, bool, Dpt1, "DPT-1.004", "Ramp")
KNX_DPT_CATALOG_ENTRY(Alarm, Boolean, 5, Boolean, bool, Dpt1, "DPT-1.005", "Alarm")
KNX_DPT_CATALOG_ENTRY(BinaryValue, Boolean, 6, Boolean, bool, Dpt1, "DPT-1.006", "Binary value")
KNX_DPT_CATALOG_ENTRY(Step, Boolean, 7, Boolean, bool, Dpt1, "DPT-1.007", "Step (up/down)")
KNX_DPT_CATALOG_ENTRY(UpDown, Boolean, 8, Boolean, bool, Dpt1, "DPT-1.008", "Up/Down")
KNX_DPT_CATALOG_ENTRY(OpenClose, Boolean, 9, Boolean, bool, Dpt1, "DPT-1.009", "Open/Close")
KNX_DPT_CATALOG_ENTRY(StartStop, Boolean, 10, Boolean, bool, Dpt1, "DPT-1.010", "Start/Stop")
KNX_DPT_CATALOG_ENTRY(State, Boolean, 11, Boolean, bool, Dpt1, "DPT-1.011", "State (active/inactive)")
KNX_DPT_CATALOG_ENTRY(Invert, Boolean, 12, Boolean, bool, Dpt1, "DPT-1.012", "Invert")
KNX_DPT_CATALOG_ENTRY(Occupancy, Boolean, 18, Boolean, bool, Dpt1, "DPT-1.018", "Occupancy")
KNX_DPT_CATALOG_ENTRY(WindowDoor, Boolean, 19, Boolean, bool, Dpt1, "DPT-1.019", "Window/Door (0=closed, 1=open)")
KNX_DPT_CATALOG_ENTRY(HeatCool, Boolean, 100, Boolean, bool, Dpt1, "DPT-1.100", "Heat/Cool (0=Cool, 1=Heat)")
KNX_DPT_CATALOG_ENTRY(Trigger, Boolean, 17, Boolean, bool, Dpt1, "DPT-1.017", "Trigger")
KNX_DPT_CATALOG_ENTRY(AlarmAck, Boolean, 16, Boolean, bool, Dpt1, "DPT-1.016", "Acknowledge")
KNX_DPT_CATALOG_ENTRY(Reset, Boolean, 15, Boolean, bool, Dpt1, "DPT-1.015", "Reset")
KNX_DPT_CATALOG_ENTRY(Controlled1Bit, Controlled1Bit, 0, Controlled1Bit, Dpt2Value, Dpt2, "DPT-2", "1-bit controlled value")
KNX_DPT_CATALOG_ENTRY(Controlled3Bit, Controlled3Bit, 0, Controlled3Bit, Dpt3Value, Dpt3, "DPT-3", "3-bit controlled value")
KNX_DPT_CATALOG_ENTRY(Dimming, Controlled3Bit, 7, Controlled3Bit, Dpt3Value, Dpt3, "DPT-3.007", "Dimming control (up/down + step code)")
KNX_DPT_CATALOG_ENTRY(Character, Character, 0, Character, char, Dpt4, "DPT-4", "8-bit character")
KNX_DPT_CATALOG_ENTRY(Unsigned8, Unsigned8, 0, Unsigned8, uint8_t, Dpt5, "DPT-5", "8-bit unsigned value")
KNX_DPT_CATALOG_ENTRY(Signed8, Signed8, 0, Signed8, int8_t, Dpt6, "DPT-6", "8-bit signed value")
KNX_DPT_CATALOG_ENTRY(CounterPulsesSigned, Signed8, 10, Signed8, int8_t, Dpt6, "DPT-6.010", "Signed counter pulses (setpoint shift steps)")
KNX_DPT_CATALOG_ENTRY(Unsigned16, Unsigned16, 0, Unsigned16, uint16_t, Dpt7, "DPT-7", "16-bit unsigned value")
KNX_DPT_CATALOG_ENTRY(Signed16, Signed16, 0, Signed16, int16_t, Dpt8, "DPT-8", "16-bit signed value")
KNX_DPT_CATALOG_ENTRY(Float2Byte, Float2Byte, 0, Float2Byte, float, Dpt9, "DPT-9", "2-byte floating point value")
KNX_DPT_CATALOG_ENTRY(TimeOfDay, TimeOfDay, 0, TimeOfDay, Dpt10Value, Dpt10, "DPT-10", "time of day")
KNX_DPT_CATALOG_ENTRY(Date, Date, 0, Date, Dpt11Value, Dpt11, "DPT-11", "date")
KNX_DPT_CATALOG_ENTRY(Unsigned32, Unsigned32, 0, Unsigned32, uint32_t, Dpt12, "DPT-12", "32-bit unsigned value")
KNX_DPT_CATALOG_ENTRY(Signed32, Signed32, 0, Signed32, int32_t, Dpt13, "DPT-13", "32-bit signed value")
KNX_DPT_CATALOG_ENTRY(Float4Byte, Float4Byte, 0, Float4Byte, float, Dpt14, "DPT-14", "4-byte floating point value")
KNX_DPT_CATALOG_ENTRY(AccessControl, AccessControl, 0, AccessControl, Dpt15Value, Dpt15, "DPT-15", "access control")
KNX_DPT_CATALOG_ENTRY(String, String, 0, String, std::string, Dpt16, "DPT-16", "string")
KNX_DPT_CATALOG_ENTRY(SceneNumber, SceneNumber, 0, SceneNumber, Dpt17Value, Dpt17, "DPT-17", "scene number")
KNX_DPT_CATALOG_ENTRY(SceneControl, SceneControl, 0, SceneControl, Dpt18Value, Dpt18, "DPT-18", "scene control")
KNX_DPT_CATALOG_ENTRY(DateTime, DateTime, 0, DateTime, Dpt19Value, Dpt19, "DPT-19", "date and time")
KNX_DPT_CATALOG_ENTRY(HvacMode, HvacMode, 0, HvacMode, Dpt20Mode, Dpt20, "DPT-20", "HVAC mode")
KNX_DPT_CATALOG_ENTRY(RgbColor, RgbColor, 0, RgbColor, Dpt232Value, Dpt232, "DPT-232", "RGB color")
KNX_DPT_CATALOG_ENTRY(XyYColor, XyYColor, 0, XyYColor, Dpt242Value, Dpt242, "DPT-242", "xyY color")
KNX_DPT_CATALOG_ENTRY(HsvColor, HsvColor, 0, HsvColor, Dpt243Value, Dpt243, "DPT-243", "HSV color")
KNX_DPT_CATALOG_ENTRY(ColorTransition, ColorTransition, 0, ColorTransition, Dpt244Value, Dpt244, "DPT-244", "color transition")

/* Common Float (2-byte) subtypes */
KNX_DPT_CATALOG_ENTRY(Temperature, Float2Byte, 1, Float2Byte, float, Dpt9, "DPT-9.001", "Temperature (°C)")
KNX_DPT_CATALOG_ENTRY(Humidity, Float2Byte, 7, Float2Byte, float, Dpt9, "DPT-9.007", "Relative humidity (%%)")
KNX_DPT_CATALOG_ENTRY(Illuminance, Float2Byte, 4, Float2Byte, float, Dpt9, "DPT-9.004", "Illuminance (lux)")
KNX_DPT_CATALOG_ENTRY(Power, Float2Byte, 24, Float2Byte, float, Dpt9, "DPT-9.024", "Active power (kW)")
KNX_DPT_CATALOG_ENTRY(Current, Float2Byte, 21, Float2Byte, float, Dpt9, "DPT-9.021", "Current (mA)")
KNX_DPT_CATALOG_ENTRY(Pressure, Float2Byte, 6, Float2Byte, float, Dpt9, "DPT-9.006", "Pressure (Pa)")
KNX_DPT_CATALOG_ENTRY(VolumeFlow, Float2Byte, 25, Float2Byte, float, Dpt9, "DPT-9.025", "Volume flow (l/h)")
KNX_DPT_CATALOG_ENTRY(Voltage, Float2Byte, 20, Float2Byte, float, Dpt9, "DPT-9.020", "Voltage (mV)")
KNX_DPT_CATALOG_ENTRY(TemperatureDelta, Float2Byte, 2, Float2Byte, float, Dpt9, "DPT-9.002", "Temperature difference (K)")
KNX_DPT_CATALOG_ENTRY(CO2, Float2Byte, 8, Float2Byte, float, Dpt9, "DPT-9.008", "CO2 / air quality (ppm)")
KNX_DPT_CATALOG_ENTRY(WindSpeed, Float2Byte, 5, Float2Byte, float, Dpt9, "DPT-9.005", "Wind speed (m/s)")
KNX_DPT_CATALOG_ENTRY(TimeSeconds, Float2Byte, 10, Float2Byte, float, Dpt9, "DPT-9.010", "Time (s)")
/* Water vapour mass per unit volume. The correct subtype for a *calculated*
 * absolute humidity; DPT-9.007 is relative humidity and carries a different
 * quantity, so the two must not share a subtype. */
KNX_DPT_CATALOG_ENTRY(AbsoluteHumidity, Float2Byte, 29, Float2Byte, float, Dpt9, "DPT-9.029", "Absolute humidity (g/m³)")

/* Common Unsigned8 (1-byte) subtypes */
KNX_DPT_CATALOG_ENTRY(Percent, Unsigned8, 1, Unsigned8, uint8_t, Dpt5, "DPT-5.001", "Percentage (0..100)")
/* No dedicated "dimming level" DPT exists; DPT-5.010 (Value_1_Ucount, an
 * unscaled 0..255 count) is the closest generic 1-byte fit. */
KNX_DPT_CATALOG_ENTRY(Level, Unsigned8, 10, Unsigned8, uint8_t, Dpt5, "DPT-5.010", "Generic 8-bit count / level (0..255)")
KNX_DPT_CATALOG_ENTRY(AngleUnsigned, Unsigned8, 3, Unsigned8, uint8_t, Dpt5, "DPT-5.003", "Angle (0..360 in 1.4118° steps)")

/* Common Unsigned16 (2-byte unsigned) subtypes */
KNX_DPT_CATALOG_ENTRY(Counter16, Unsigned16, 1, Unsigned16, uint16_t, Dpt7, "DPT-7.001", "Unsigned 16-bit counter")
KNX_DPT_CATALOG_ENTRY(TimeMs16, Unsigned16, 2, Unsigned16, uint16_t, Dpt7, "DPT-7.002", "Time / period (ms)")
/* Colour temperature has no 1-byte DPT; DPT-7.600 (2-byte unsigned, Kelvin)
 * is the standard KNX representation for tunable-white lighting. */
KNX_DPT_CATALOG_ENTRY(ColorTemperature, Unsigned16, 600, Unsigned16, uint16_t, Dpt7, "DPT-7.600", "Absolute colour temperature (K)")

/* Common Signed16 (2-byte signed) subtypes */
KNX_DPT_CATALOG_ENTRY(Delta16, Signed16, 1, Signed16, int16_t, Dpt8, "DPT-8.001", "Signed 16-bit delta")

/* Common Signed32 (4-byte signed) subtypes */
KNX_DPT_CATALOG_ENTRY(Energy, Signed32, 10, Signed32, int32_t, Dpt13, "DPT-13.010", "Active electrical energy (Wh)")

/* Common Float4Byte (4-byte float) subtypes */
KNX_DPT_CATALOG_ENTRY(Float4_Temperature, Float4Byte, 68, Float4Byte, float, Dpt14, "DPT-14.068", "High-precision temperature (°C)")
/* Gas resistance value (M-ohm range) overflows the DPT-9 2-byte float, so a
 * 4-byte float is required; DPT-14.060 (Value_Resistance) is the correctly
 * identified KNX subtype for it (full float range, no overflow concern). */
KNX_DPT_CATALOG_ENTRY(Resistance, Float4Byte, 60, Float4Byte, float, Dpt14, "DPT-14.060", "Resistance (Ω)")
KNX_DPT_CATALOG_ENTRY(Frequency, Float4Byte, 33, Float4Byte, float, Dpt14, "DPT-14.033", "Frequency (Hz)")
KNX_DPT_CATALOG_ENTRY(PowerFactor, Float4Byte, 57, Float4Byte, float, Dpt14, "DPT-14.057", "Power factor (cos φ)")

/* HVAC control enumerations — DPT 20 (N8, 1-byte enum). Values below are the
 * authoritative KNX enumerations from KNX Spec v2.1, Vol 3/7.2 "Datapoint
 * Types" (§DPT_HVACMode …DPT_StatusRoomSetp). Only 20.102 has a fixed C++ enum
 * (Dpt20Mode) and a range-checked codec (Dpt20) in this stack; the other
 * subtypes are carried as a raw 1-byte value (codec Dpt5) so a controller can
 * assign the documented meaning per subtype without a bespoke codec each. The
 * DPST-20-nnn id is what ETS keys on regardless. */
KNX_DPT_CATALOG_ENTRY(HvacModeComfort, HvacMode, 102, HvacMode, Dpt20Mode, Dpt20, "DPT-20.102", "HVAC operating mode (0=Auto 1=Comfort 2=Standby 3=Economy 4=Building Protection)")
KNX_DPT_CATALOG_ENTRY(HvacDhwMode, HvacMode, 103, Unsigned8, uint8_t, Dpt5, "DPT-20.103", "DHW mode (0=Auto 1=LegioProtect 2=Normal 3=Reduced 4=Off/FrostProtect)")
KNX_DPT_CATALOG_ENTRY(HvacLoadPriority, HvacMode, 104, Unsigned8, uint8_t, Dpt5, "DPT-20.104", "Load priority (0=None 1=ShiftLoadPriority 2=AbsoluteLoadPriority)")
KNX_DPT_CATALOG_ENTRY(HvacContrMode, HvacMode, 105, Unsigned8, uint8_t, Dpt5, "DPT-20.105", "HVAC controller mode (0=Auto 1=Heat 2=MorningWarmup 3=Cool 4=NightPurge 5=Precool 6=Off 7=Test 8=EmergHeat 9=FanOnly 10=FreeCool 11=Ice 12=MaxHeat 13=EconHeatCool 14=Dehumid 15=Calib 16=EmergCool 17=EmergSteam 20=NoDemand)")
KNX_DPT_CATALOG_ENTRY(HvacEmergMode, HvacMode, 106, Unsigned8, uint8_t, Dpt5, "DPT-20.106", "HVAC emergency mode (0=Normal 1=EmergPressure 2=EmergDepressure 3=EmergPurge 4=EmergShutdown 5=EmergFire)")
KNX_DPT_CATALOG_ENTRY(HvacChangeoverMode, HvacMode, 107, Unsigned8, uint8_t, Dpt5, "DPT-20.107", "Heat/cool changeover mode (0=Auto 1=CoolingOnly 2=HeatingOnly)")
KNX_DPT_CATALOG_ENTRY(HvacValveMode, HvacMode, 108, Unsigned8, uint8_t, Dpt5, "DPT-20.108", "Valve mode (1=HeatStageA 2=HeatStageB 3=CoolStageA 4=CoolStageB 5=Heat/Cool changeover)")
KNX_DPT_CATALOG_ENTRY(HvacDamperMode, HvacMode, 109, Unsigned8, uint8_t, Dpt5, "DPT-20.109", "Damper mode (1=FreshAir 2=SupplyAir 3=ExtractAir 4=ExtractAir)")
KNX_DPT_CATALOG_ENTRY(HvacHeaterMode, HvacMode, 110, Unsigned8, uint8_t, Dpt5, "DPT-20.110", "Heater mode (1=HeatStageA On/Off 2=HeatStageA Proportional 3=HeatStageB Proportional)")
KNX_DPT_CATALOG_ENTRY(HvacFanMode, HvacMode, 111, Unsigned8, uint8_t, Dpt5, "DPT-20.111", "Fan mode (0=NotRunning 1=PermanentlyRunning 2=RunningInIntervals)")
KNX_DPT_CATALOG_ENTRY(HvacMasterSlaveMode, HvacMode, 112, Unsigned8, uint8_t, Dpt5, "DPT-20.112", "Master/slave mode (0=Autonomous 1=Master 2=Slave)")
KNX_DPT_CATALOG_ENTRY(HvacStatusRoomSetp, HvacMode, 113, Unsigned8, uint8_t, Dpt5, "DPT-20.113", "Room setpoint status (0=NormalSetpoint 1=AlternativeSetpoint 2=BuildingProtectionSetpoint)")

/* HVAC status words — DPT 21 (B8) / 22 (B16) bit-sets. Carried as a raw
 * unsigned integer; pack the documented bits yourself. Bit numbers from KNX
 * Spec v2.1, Vol 3/7.2. */
/* The generic per-Datapoint status octet ("Z8"). KNX Vol 7 sensor Functional
 * Blocks (RTS, FTS, RRHS, RAQS, ...) pair every measurement Output with an
 * optional StatusGO of this type, which is how a sensor reports its own health
 * on the bus without a bespoke, product-private status word. */
KNX_DPT_CATALOG_ENTRY(StatusGen, Bitfield8, 1, Unsigned8, uint8_t, Dpt5, "DPT-21.001", "Generic Datapoint status (b0=OutOfService b1=Fault b2=Overridden b3=InAlarm b4=AlarmUnAck)")
KNX_DPT_CATALOG_ENTRY(StatusRHC, Bitfield8, 102, Unsigned8, uint8_t, Dpt5, "DPT-21.102", "Room H/C controller status (b0=Fault b1=StatusECO b2=TempFlowLimit b3=TempReturnLimit b4=StatusMorningBoost b5=StatusStartOptim b6=StatusStopOptim b7=SummerMode)")
KNX_DPT_CATALOG_ENTRY(StatusRHCC, Bitfield16, 101, Unsigned16, uint16_t, Dpt7, "DPT-22.101", "Room H/C controller status (b0=Fault b1=StatusEcoH b2=TempFlowLimit b3=TempReturnLimit b4=StatusMorningBoostH b5=StatusStartOptim b6=StatusStopOptim b7=HeatingDisabled b8=HeatCoolMode(1=heat) b9=StatusEcoC b10=StatusPreCool b11=CoolingDisabled b13=FrostAlarm b14=OverheatAlarm)")

/* Common String subtypes */
KNX_DPT_CATALOG_ENTRY(Label, String, 1, String, std::string, Dpt16, "DPT-16.001", "Short label / name")