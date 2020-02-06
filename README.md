### TriggerTimer
Arduino-based JK HLS Laser trigger delay system

Version 0.0.1

#### Features: 

* Takes (hardware interrupt on pin 2) input trigger pulse "OSC SYNC" from laser when flash lamps are set off. 
  Three programmable delayed triggers are then output on pins 3, 4 and 5
* delays can be programmed via a command on the serial port or via MQTT over LAN. 
* Command in JSON format: {"Delays":[1200,000,380]} corresponding to Delay1, Delay2, DelayAMP (in microseconds) 
* Delays are stored in EEPROM for persistency
