/*

UKHASnet Repeater Code by Phil Crump M0DNY
Based on UKHASnet rf69_repeater by James Coxon M6JCX

The MIT License (MIT)

Copyright (c) 2014 Phil Crump (unless otherwise attributed)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Crimes in programming have been committed by Chris Stubbs in a futile attempt to have this work with the ESP8266 WiFi SoC


*/

#include <SPI.h>
#include <string.h>
#include "RFM69Config.h"
#include "RFM69.h"
#include "LowPower.h"
#include "NodeConfig.h"
#include "ESP8266.h"
#include "wifiConfig.h" //this is where you need to set your SSID and Password
#define DST_IP "212.71.255.157" //ukhas.net 212.71.255.157
#define WIFI_EN 7 //CH_PD


//************* Misc Setup ****************/
ESP8266 esp8266 = ESP8266();
float battV=0.0;
uint8_t n;
uint32_t count = 1, data_interval = 2; // Initially send a couple of beacons in quick succession
uint8_t zombie_mode; // Stores current status: 0 - Full Repeating, 1 - Low Power shutdown, (beacon only)
uint8_t data_count = 97; // 'a'
char data[64], string_end[] = "]";

// Singleton instance of the radio
RFM69 rf69(RFM_TEMP_FUDGE); // parameter: RFM temperature calibration offset (degrees as float)

#ifdef ENABLE_RFM_TEMPERATURE
int8_t sampleRfmTemp() {
    int8_t rfmTemp = rf69.readTemp();
    while(rfmTemp>100) {
        rfmTemp = rf69.readTemp();
    }
    if(zombie_mode==0) {
        rfmTemp-=RX_TEMP_FUDGE;
    }
    return rfmTemp;
}
#endif

#ifdef ENABLE_BATTV_SENSOR
float sampleBattv() {
  // External 4:1 Divider
  return ((float)analogRead(BATTV_PIN)*1.1*4*BATTV_FUDGE)/1023.0;
}
#endif

int gen_Data(){
  if(data_count=='a' or data_count=='z') {
      sprintf(data, "%c%cL%s", num_repeats, data_count, location_string);
  } else {
      sprintf(data, "%c%c", num_repeats, data_count);
  }
  
  #ifdef ENABLE_RFM_TEMPERATURE
  sprintf(data,"%sT%d",data,sampleRfmTemp());
  #endif
  
  #ifdef ENABLE_BATTV_SENSOR
  battV = sampleBattv();
  char* battStr;
  char tempStrB[14]; //make buffer large enough for 7 digits
  battStr = dtostrf(battV,7,2,tempStrB);
  while( (strlen(battStr) > 0) && (battStr[0] == 32) )
  {
     strcpy(battStr,&battStr[1]);
  }
  sprintf(data,"%sV%s",data,battStr);
  #endif
  
  #ifdef ENABLE_ZOMBIE_MODE
  sprintf(data,"%sZ%d",data,zombie_mode);
  #endif
  
  return sprintf(data,"%s[%s]",data,id);
}

void setup() 
{
  Serial.begin(9600); //Open serial communications 
  esp8266.initialise(Serial, WIFI_EN); //Pass it over to the ESP8266 class, along with the pin number to enable the module (CH_PD)
  while (!esp8266.resetModule()); //reset module until it is ready
  esp8266.tryConnectWifi(SSID, PASS);//connect to the wifi
  esp8266.singleConnectionMode(); //set the single connection mode
  
  #ifdef STAT_LEDS
  pinMode(RXLED, OUTPUT);
  pinMode(TXLED, OUTPUT);
  #endif
  
  analogReference(INTERNAL); // 1.1V ADC reference
  randomSeed(analogRead(6));
  delay(1000);
  
  while (!rf69.init()){
    delay(100);
  }
  
  
  
  int packet_len = gen_Data();
  rf69.send((uint8_t*)data, packet_len, rfm_power);
  
  #ifdef ENABLE_ZOMBIE_MODE
  if(battV > ZOMBIE_THRESHOLD) {
    rf69.setMode(RFM69_MODE_RX);
    zombie_mode=0;
  } else {
    rf69.setMode(RFM69_MODE_SLEEP);
    zombie_mode=1;
  }
  #else
  rf69.setMode(RFM69_MODE_RX);
  zombie_mode=0;
  #endif
  
    #ifdef SENSITIVE_RX
    rf69.SetLnaMode(RF_TESTLNA_SENSITIVE);
    #endif
  
}

void loop()
{
  count++;
  
  if(zombie_mode==0) {
    rf69.setMode(RFM69_MODE_RX);
    
    for(int i=0;i<64;i++) {
      LowPower.powerDown(SLEEP_120MS, ADC_OFF, BOD_OFF);
      
      if (rf69.checkRx()) {
        uint8_t buf[64];
        uint8_t len = sizeof(buf);
        
        rf69.recv(buf, &len);
        
        #ifdef STAT_LEDS
        digitalWrite(RXLED, HIGH);
        delay(100);
        digitalWrite(RXLED, LOW);
        #endif
        
        delay(500); //delay for safe keeping
        
        //Build up the string to upload to server
        String uploadPacket = "origin=";
        uploadPacket += id; //gateway nodes ID
        uploadPacket += "&data=";
        
        for (int i = 0; i < len-1; i++){ 
          uploadPacket += char(buf[i]); //copy the packet from the buffer we got from rf69.recv into our upload string. There may be neater ways of doing this.
        }
        
        uploadPacket += "&rssi=";      
        uploadPacket += String(rf69.lastRssi());
        uploadPacket += "\0"; //null terminate the string for safe keeping
        esp8266.uploadPacket(DST_IP, uploadPacket);
        

        // find end of packet & start of repeaters
        int end_bracket = -1, start_bracket = -1;        
        for (int i=0; i<len; i++) {
          if (buf[i] == '[') {
            start_bracket = i;
          }
          else if (buf[i] == ']') {
            end_bracket = i;
            buf[i+1] = '\0';
            break;
          }
        }

        // Need to take the recieved buffer and decode it and add a reference 
        if (buf[0] > '0' && end_bracket != -1 && strstr((const char *)&buf[start_bracket], id) == NULL) {
          // Reduce the repeat value
          buf[0]--;
          
          // Add the repeater ID
          int packet_len = end_bracket + sprintf((char *)&buf[end_bracket], ",%s]", id);

          //random delay to try and avoid packet collision
          delay(random(50, 800));
          
          rf69.send((uint8_t*)buf, packet_len, rfm_power);
          
        #ifdef STAT_LEDS
        digitalWrite(TXLED, HIGH);
        delay(100);
        digitalWrite(TXLED, LOW);
        #endif
          
        }
      }
    }
  } else {
    // Sample Sensors here..
    
    // Sleep for 8 seconds
    rf69.setMode(RFM69_MODE_SLEEP);
    LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
  }
  
  if (count >= data_interval){
    data_count++;

    if(data_count > 122){
      data_count = 98; //'b'
    }
    
    int packet_len = gen_Data();
    rf69.send((uint8_t*)data, packet_len, rfm_power);
    
    data_interval = random(BEACON_INTERVAL, BEACON_INTERVAL+10) + count;
    #ifdef ENABLE_ZOMBIE_MODE
    if(battV > ZOMBIE_THRESHOLD && zombie_mode==1) {
        rf69.setMode(RFM69_MODE_RX);
        zombie_mode=0;
    } else if (battV < ZOMBIE_THRESHOLD && zombie_mode==0) {
        rf69.setMode(RFM69_MODE_SLEEP);
        zombie_mode=1;
    }
    #endif
    #ifdef SENSITIVE_RX
    rf69.SetLnaMode(RF_TESTLNA_SENSITIVE);
    #endif
  }
}
