
/*
Trigger delay system for JK HLS lasers
(c) Loic74 <loic74650@gmail.com> 2020

***Dependencies and respective revisions used to compile this project***
https://github.com/256dpi/arduino-mqtt/releases (rev 2.4.3)
https://github.com/prampec/arduino-softtimer (rev 3.1.3)
https://github.com/bblanchon/ArduinoJson (rev 5.13.4)
https://github.com/sdesalas/Arduino-Queue.h (rev )

*/
#include <Ethernet.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <Queue.h>
#include <EEPROMex.h>
#include <Streaming.h>

// Firmware revision
String Firmw = "0.0.1";

//Version of config stored in Eeprom
//Random value. Change this value (to any other value) to revert the config to default values
#define CONFIG_VERSION 116

//Starting point address where to store the config data in EEPROM
#define memoryBase 32
int configAdress=0;
const int maxAllowedWrites = 200;//not sure what this is for

String command;

//Queue object to store incoming JSON commands (up to 5)
Queue<String> queue = Queue<String>(5);

//buffers for MQTT string payload
#define PayloadBufferLength 128
char Payload[PayloadBufferLength];

//Settings structure and its default values
struct StoreStruct 
{
    uint8_t ConfigVersion;   // This is for testing if first time using eeprom or not
    unsigned int Delay1, Delay2, DelayAMP;
} storage = 
{   //default values. Change the value of CONFIG_VERSION in order to restore the default values
    CONFIG_VERSION,
    1201, 1, 381
};

// MAC address of Ethernet shield (in case of Controllino board, set an arbitrary MAC address)
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
String sArduinoMac;
IPAddress ip(192, 168, 0, 21);  //IP address, needs to be adapted depending on local network topology
EthernetClient net;             //Ethernet client to connect to MQTT server

//MQTT stuff including local broker/server IP address, login and pwd
MQTTClient MQTTClient;
const char* MqttServerIP = "192.168.0.38";
const char* MqttServerClientID = "ArduinoTrigger"; // /!\ choose a client ID which is unique to this Arduino board
const char* MqttServerLogin = "admin";  //replace by const char* MqttServerLogin = nullptr; in case broker does not require a login/pwd
const char* MqttServerPwd = "XXXXX"; //replace by const char* MqttServerPwd = nullptr; in case broker does not require a login/pwd
const char* TrigTopic = "Hololab/Trigger";
const char* TrigTopicAPI = "Hololab/Trigger/API";
const char* TrigTopicStatus = "Hololab/Trigger/status";

//serial printing stuff
String _endl = "\n";

bool Trigged = false;
const byte ledPin = 13;
const byte Osc_TrigInPin = 2; //Input trigger from OSC SYNC
const byte PC1_TrigOutPin = 3;//Output trigger to PC1
const byte PC2_TrigOutPin = 4;//Output trigger to PC2
const byte AMP_TrigOutPin = 5;//Output trigger to AMP

volatile byte state = LOW;
volatile unsigned int PulseWidth = 5;
volatile unsigned int delay1 = 1270;
volatile unsigned int delay2 = 0;
volatile unsigned int delayAMP = 270;
volatile unsigned int delayPC = delay1 - delayAMP - PulseWidth;

void setup() {
  // put your setup code here, to run once:
  pinMode(ledPin, OUTPUT);
  pinMode(PC1_TrigOutPin, OUTPUT);
  pinMode(PC2_TrigOutPin, OUTPUT);
  pinMode(AMP_TrigOutPin, OUTPUT);
  pinMode(Osc_TrigInPin, INPUT_PULLUP);
  
  digitalWrite(PC1_TrigOutPin, LOW);
  digitalWrite(PC2_TrigOutPin, LOW);
  digitalWrite(AMP_TrigOutPin, LOW);
  
  attachInterrupt(digitalPinToInterrupt(Osc_TrigInPin),interrupt,FALLING);
  
  Serial.begin(9600);
  delay(200);

  // initialize Ethernet device  
  Ethernet.begin(mac, ip); 

  //Init MQTT
  MQTTClient.setOptions(60,false,10000);
  MQTTClient.setWill(TrigTopicStatus,"offline",true,LWMQTT_QOS1);
  MQTTClient.begin(MqttServerIP, net);
  MQTTClient.onMessage(messageReceived);
  MQTTConnect();

  //display remaining RAM space. For debug
  Serial<<F("[memCheck]: ")<<freeRam()<<F("b")<<_endl;

  //Initialize Eeprom
  EEPROM.setMemPool(memoryBase, EEPROMSizeATmega328); 

  //Get address of "ConfigVersion" setting
  configAdress = EEPROM.getAddress(sizeof(StoreStruct));

  //Read ConfigVersion. If does not match expected value, restore default values
  uint8_t vers = EEPROM.readByte(configAdress);

  if(vers == CONFIG_VERSION) 
  {
    Serial<<F("Stored config version: ")<<CONFIG_VERSION<<F(". Loading settings from eeprom")<<_endl;
    loadConfig();//Restore stored values from eeprom
  }
  else
  {
    Serial<<F("Stored config version: ")<<CONFIG_VERSION<<F(". Loading default settings, not from eeprom")<<_endl;
    saveConfig();//First time use. Save default values to eeprom
  }

}

void loop() {
  // put your main code here, to run repeatedly:

  if(Trigged)
  {
    Serial.println("Trigged!");
    Trigged = 0;
    digitalWrite(ledPin, HIGH);
    delay(200);
    digitalWrite(ledPin, LOW);
  }

  //Update MQTT thread
  MQTTClient.loop(); 

  //Process queued incoming JSON commands if any
  if(queue.count()>0)
    ProcessCommand(queue.pop());

  if (!MQTTClient.connected()) 
  {
    //MQTTConnect();
    //Serial.println("MQTT reconnecting...");
  }

  if(Serial.available())
  {
        command = Serial.readStringUntil('\n');        
        ProcessCommand(command);
  }
}

void interrupt()
{
  delayMicroseconds(delayAMP);
  
  digitalWrite(AMP_TrigOutPin, HIGH);
  delayMicroseconds(PulseWidth);
  digitalWrite(AMP_TrigOutPin, LOW);
  
  delayMicroseconds(delayPC);
  
  digitalWrite(PC1_TrigOutPin, HIGH);
  digitalWrite(PC2_TrigOutPin, HIGH);
  delayMicroseconds(PulseWidth);
  digitalWrite(PC1_TrigOutPin, LOW);
  digitalWrite(PC2_TrigOutPin, LOW);
  
  Trigged=1;
  EIFR = 0x01;  //clear interrupt queue
}

//Connect to MQTT broker and subscribe to the TrigTopicAPI topic in order to receive future commands
//then publish the "online" message on the "status" topic. If Ethernet connection is ever lost
//"status" will switch to "offline". Very useful to check that the Arduino is alive and functional
void MQTTConnect() 
{
  MQTTClient.connect(MqttServerClientID, MqttServerLogin, MqttServerPwd);
/*  int8_t Count=0;
  while (!MQTTClient.connect(MqttServerClientID, MqttServerLogin, MqttServerPwd) && (Count<4))
  {
    Serial<<F(".")<<_endl;
    delay(500);
    Count++;
  }
*/
  if(MQTTClient.connected())
  {
    //String TrigTopicAPI = "Hololab/Trigger/API";
    //Topic to which send/publish API commands for the Pool controls
    MQTTClient.subscribe(TrigTopicAPI);
  
    //tell status topic we are online
    if(MQTTClient.publish(TrigTopicStatus,"online",true,LWMQTT_QOS1))
      Serial<<F("published: Hololab/Trigger/status - online")<<_endl;
    else
    {
      Serial<<F("Unable to publish on status topic; MQTTClient.lastError() returned: ")<<MQTTClient.lastError()<<F(" - MQTTClient.returnCode() returned: ")<<MQTTClient.returnCode()<<_endl;
    }
  }
  else
  Serial<<F("Failed to connect to the MQTT broker")<<_endl;
  
}

//MQTT callback
//This function is called when messages are published on the MQTT broker on the TrigTopicAPI topic to which we subscribed
//Add the received command to a message queue for later processing and exit the callback
void messageReceived(String &topic, String &payload) 
{
  String TmpStr(TrigTopicAPI);

  //commands. This check might be redundant since we only subscribed to this topic
  if(topic == TmpStr)
  {
    queue.push(payload); 
    Serial<<"FreeRam: "<<freeRam()<<" - Qeued messages: "<<queue.count()<<_endl;
  }
}

void ProcessCommand(String JSONCommand)
{
        //Json buffer
      StaticJsonBuffer<200> jsonBuffer;
      
      //Parse Json object and find which command it is
      JsonObject& command = jsonBuffer.parseObject(JSONCommand);
      
      // Test if parsing succeeds.
      if (!command.success()) 
      {
        Serial<<F("Json parseObject() failed");
        return;
      }
      else
      {
        Serial<<F("Json parseObject() success - ")<<endl;

        //{"Delays":[1200,000,380]}   Delay1, Delay2, DelayAMP
        if (command.containsKey("Delays"))
        {
          unsigned int Delays[3];
          int NbPoints = command["Delays"].as<JsonArray>().copyTo(Delays);
          delay1 = storage.Delay1 = Delays[0];
          delay2 = storage.Delay2 = Delays[1];
          delayAMP = storage.DelayAMP = Delays[2];
          saveConfig();

          Serial<<F("Delays command - ")<<NbPoints<<F(" delays received: ")<<_endl;
          Serial<<F("Delay1: ")<<Delays[0]<<_endl;
          Serial<<F("Delay2: ")<<Delays[1]<<_endl;
          Serial<<F("DelayAMP: ")<<Delays[2]<<_endl;
        } 
      }
}

bool loadConfig() 
{
  EEPROM.readBlock(configAdress, storage);

  Serial<<storage.ConfigVersion<<'\n';
  Serial<<storage.Delay1<<", "<<storage.Delay2<<", "<<storage.DelayAMP<<'\n';
  delay1 = storage.Delay1;
  delay2 = storage.Delay2;
  delayAMP = storage.DelayAMP;
  
  return (storage.ConfigVersion == CONFIG_VERSION);
}

void saveConfig() 
{
   //update function only writes to eeprom if the value is actually different. Increases the eeprom lifetime
   EEPROM.writeBlock(configAdress, storage);
}

//Compute free RAM
//useful to check if it does not shrink over time
int freeRam () {
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}
