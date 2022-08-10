#include <BLEDevice.h>
#include <Esp.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>

static WiFiClient espWifiClient;
static PubSubClient MQTTClient(espWifiClient);
static BLEClient *pClient;
static BLERemoteCharacteristic *pRemoteCharacteristic_THB;
static BLERemoteService *pRemoteService;
class MiTHDevice
{
public:
	std::string address;
	std::string topic;
	float temp;
	float humi;
	float batt;
};

void connectWifi();
void reconnectToMQTT();
bool connectSensor(BLEAddress address);
void notifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic_THB, uint8_t *pData, size_t length, bool isNotify);
void disconnectDevice();

// todo: wifi ssid 密码
// ssid password
const char *ssid = "SSID";
const char *password = "PASSWORD";
// todo: MQTT服务器地址
// MQTT Server
const char *mqttServer = "192.168.31.50";
// todo: 传感器配置
// your device
// MAC, MQTT Topic, temp(default), temp(default) , temp(default)
static MiTHDevice miTHDevices[] = {
	{"a4:c1:38:00:00:00", "mqtt/topic1", -1, -1, -1},
	{"a4:c1:38:00:00:00", "mqtt/topic2", -1, -1, -1}
};
// todo: 设备数量
// device count
int deviceCount = 2;
// todo: 控制读取延时 单位 秒
const unsigned int publishGap = 5 * 60 * 1000;
bool isCanPublish = false;
// BLE 相关uuid
const BLEUUID serviceUUID("ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6");
const BLEUUID charUUID("ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6");
bool isReceiveNotification = false;
// mqtt 消息
#define MSG_BUFFER_SIZE 100
char MQTTMsg[MSG_BUFFER_SIZE];
int currentIndex = -1;
// 双核执行不同的任务
void core0Task(void *pvParameters);
void core1Task(void *pvParameters);

void setup()
{
	WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //关闭低电压检测,避免无限重启
	Serial.begin(115200);
	Serial.println("booting MiHTSensor reader");
	connectWifi();
	BLEDevice::init("ESP32");

	//初始化MQTT客户端
	MQTTClient.setServer(mqttServer, 1883);

	//初始化BLE客户端
	pClient = BLEDevice::createClient();

	//开启双核任务
	xTaskCreatePinnedToCore(core0Task, "core1Task", 10000, NULL, 2, NULL, 0);
	xTaskCreatePinnedToCore(core1Task, "core2Task", 10000, NULL, 1, NULL, 1);
}

//连接wifi
void connectWifi()
{
	Serial.println();
	Serial.print("wifi connecting...");
	WiFi.begin(ssid, password);
	int i = 0;
	while (WiFi.status() != WL_CONNECTED)
	{
		i++;
		delay(1000);
		Serial.print(".");
		if (i > 20)
		{
			Serial.println("fail to cannect, restarting...");
			ESP.restart();
		}
	}
	Serial.print("\n");
	Serial.println("---wifi connected---");
}

void reconnectToMQTT()
{
	int retryCount = 0;
	while (!MQTTClient.connected())
	{
		if (retryCount > 15)
		{
			Serial.println("failed to cannect to MQTT, restarting...");
			ESP.restart();
		}
		Serial.print("Attempting MQTT connection...");
		const String clientId = "ESP32";
		if (MQTTClient.connect(clientId.c_str()))
		{
			Serial.println("connected to MQTT");
		}
		else
		{
			Serial.print("failed: ");
			Serial.print(MQTTClient.state());
			Serial.println(" try again in 5 seconds");
			retryCount++;
			delay(5000);
		}
	}
}

void loop()
{
}

// 核心1 做mqtt发布
void core1Task(void *pvParameters)
{
	while (true)
	{
		if (!MQTTClient.connected())
		{
			reconnectToMQTT();
			continue;
		}
		MQTTClient.loop();

		if (isCanPublish)
		{
			isCanPublish = false;
			// 判断wifi状态
			if (WiFi.status() != WL_CONNECTED)
			{
				Serial.println("wifi disconnected restarting...");
				disconnectDevice();
				ESP.restart();
			}

			Serial.println("publishing to mqtt server...");
			for (int i = 0; i < deviceCount; i++)
			{
				if (miTHDevices[i].batt == -1)
					continue;
				snprintf(MQTTMsg,
						 MSG_BUFFER_SIZE,
						 "{\"temperature\":\"%.2lf\",\"humidity\":\"%.2lf\",\"battery\":\"%.2lf\"}",
						 miTHDevices[i].temp,
						 miTHDevices[i].humi,
						 miTHDevices[i].batt);
				MQTTClient.publish(miTHDevices[i].topic.c_str(), MQTTMsg);
				Serial.println("Published: ");
				Serial.println(miTHDevices[i].topic.c_str());
				Serial.println(MQTTMsg);
			}
		}
		else
		{
			delay(1000);
		}
	}
}
//核心0用作获取数据
void core0Task(void *pvParameters)
{
	while (true)
	{
		//对于每一个address
		for (int i = 0; i < deviceCount; i++)
		{
			currentIndex = i;
			int retryCount = 0;
			bool success = true;
			Serial.printf("connecting to %s\n", miTHDevices[i].address.c_str());
			BLEAddress bleAddress(miTHDevices[i].address);
			//尝试三次连接
			while (!connectSensor(bleAddress))
			{
				retryCount++;
				if (retryCount >= 3)
				{
					success = false;
					Serial.println("---all 3 times failed---");
					break;
				}
				delay(5000);
			}
			//判断连接状态
			if (success)
			{
				Serial.println("sensor connected, waiting for notification");
				//"阻塞"与超时
				retryCount = 0;
				while (!isReceiveNotification)
				{
					delay(1000);
					retryCount++;
					//超时未响应
					if (retryCount >= 20)
					{
						Serial.println("there is no reply from sensor, disconnecting");
						disconnectDevice();
						break;
					}
				}
				//运行到这里说明已经好了

				if (pClient->isConnected())
				{
					disconnectDevice();
					do
					{
						delay(1000);
					} while (pClient->isConnected());
				}
			}
			//每个传感器之间缓一缓
			delay(1000);
			if (i + 1 == deviceCount)
			{
				isCanPublish = true;
				delay(publishGap);
			}
		}
	}
}

bool connectSensor(BLEAddress address)
{
	pClient->connect(address);
	if (pClient->isConnected() == true)
	{
		Serial.print("connected to ");
		Serial.print(pClient->getPeerAddress().toString().c_str());
		Serial.printf("\nsignal strength= %d \n", pClient->getRssi());

		pRemoteService = pClient->getService(serviceUUID);
		if (pRemoteService == nullptr)
		{
			Serial.print("connection broken: fail to get service");
			pClient->disconnect();
			return false;
		}
		pRemoteCharacteristic_THB = pRemoteService->getCharacteristic(charUUID);
		if (pRemoteCharacteristic_THB == nullptr)
		{
			Serial.print("connection broken: fail to get characteristic THB");
			pClient->disconnect();
			return false;
		}
		else
		{
			isReceiveNotification = false;
			pRemoteCharacteristic_THB->registerForNotify(notifyCallback);
			return true;
		}
	}
	else
	{
		Serial.println("fail to connect");
		return false;
	}
}

//收到notification之后的回调
void notifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic_THB, uint8_t *pData, size_t length, bool isNotify)
{
	Serial.println("data received");
	float temp, humi, batt;
	temp = (pData[0] | (pData[1] << 8)) * 0.01;
	humi = pData[2];
	batt = (pData[3] | (pData[4] << 8)) * 0.001;
	Serial.printf("TEMP %.2f C - HUMI %.2f %% - BATT= %.3f V - free heap= %d\n", temp, humi, batt, esp_get_free_heap_size());
	miTHDevices[currentIndex].temp = temp;
	miTHDevices[currentIndex].humi = humi;
	miTHDevices[currentIndex].batt = batt;
	isReceiveNotification = true;
}

void disconnectDevice()
{
	uint8_t val[] = {0x00, 0x00};
	pRemoteCharacteristic_THB->getDescriptor((uint16_t)0x2902)->writeValue(val, 2, true);
	if (pClient != NULL)
		if (pClient->isConnected())
			pClient->disconnect();
}