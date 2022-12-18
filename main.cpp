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

struct MiTHDevice
{
	std::string deviceName;
	std::string address;
	std::string topic;
	float temp;
	float humi;
	float batt;
};

void connectWifi();
void reconnectToMQTT();
void sendToServer(std::string deviceName, std::string mqttTopic, char *jsonData);
bool connectAndRead(BLEAddress address);
void notifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic_THB, uint8_t *pData, size_t length, bool isNotify);
void disconnectDevice();
void sendMiTHData(int deviceIndex);
void taskReadSensor(void *pvParameters);

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
MiTHDevice miTHDevices[] = {
	{"Mijia01", "a4:c1:38:00:00:00", "device/MiTH01", -1, -1, -1},
	{"Mijia02", "a4:c1:38:00:00:01", "device/MiTH02", -1, -1, -1}};
// todo: 温湿度设备数量
// Mi TH device count
const int countHTSensor = 2;
int indexCurrentSensor = -1;
// todo: 控制读取延时 单位 毫秒
// todo: publish gap (ms)
const unsigned int publishGap = 2 * 60 * 1000;
// const unsigned int publishGap = 5 * 1000;
// BLE 相关uuid
const BLEUUID serviceUUID("ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6");
const BLEUUID charUUID("ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6");
bool isReceiveNotification = false;
// mqtt 消息buffer
#define JSON_BUFFER_SIZE 200
char bufJsonData[JSON_BUFFER_SIZE];
// 最大尝试次数
const int MAX_RETRY = 3;

void setup()
{
	WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // 关闭低电压检测 避免重启问题
	Serial.begin(115200);
	Serial.println("booting");

	// 初始化wifi
	connectWifi();

	// 初始化MQTT客户端
	MQTTClient.setServer(mqttServer, 1883);
	MQTTClient.loop();

	// 初始化BLE客户端
	BLEDevice::init("ESP32");
	pClient = BLEDevice::createClient();

	// 开启任务
	xTaskCreatePinnedToCore(taskReadSensor, "taskReadSensor", 8192, NULL, 2, NULL, 1);
}

// 连接wifi
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
	Serial.println("wifi connected");
}

void reconnectToMQTT()
{
	int retryCount = 0;
	while (!MQTTClient.connected())
	{
		if (retryCount > MAX_RETRY)
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
	MQTTClient.loop();
	delay(1000);
}

void sendMiTHData(int deviceIndex)
{
	if (miTHDevices[deviceIndex].batt != -1)
	{
		snprintf(bufJsonData,
				 JSON_BUFFER_SIZE,
				 "{\"deviceName\":\"%s\",\"temperature\":\"%.2lf\",\"humidity\":\"%.2lf\",\"battery\":\"%.2lf\"}",
				 miTHDevices[deviceIndex].deviceName.c_str(),
				 miTHDevices[deviceIndex].temp,
				 miTHDevices[deviceIndex].humi,
				 miTHDevices[deviceIndex].batt);

		sendToServer(miTHDevices[deviceIndex].deviceName, miTHDevices[deviceIndex].topic, bufJsonData);
	}
}

void sendToServer(std::string deviceName, std::string mqttTopic, char *jsonData)
{
	if (!WiFi.isConnected())
	{
		Serial.println("WIFI disconnected, reconnecting...");
		connectWifi();
	}

	if (!MQTTClient.connected())
	{
		reconnectToMQTT();
	}

	Serial.println("publishing to server...");
	MQTTClient.publish(mqttTopic.c_str(), jsonData);

	Serial.println("sent: ");
	Serial.printf("deviceName: %s\n", deviceName.c_str());
	Serial.printf("mqttTopic: %s\n", mqttTopic.c_str());
	Serial.printf("jsonData: %s\n", jsonData);
}

void taskReadSensor(void *pvParameters)
{
	char co2Buffer[100] = {0};
	int retryCount = 0;
	for (;;)
	{
		// 对于每一个address
		for (indexCurrentSensor = 0; indexCurrentSensor < countHTSensor; indexCurrentSensor++)
		{
			retryCount = 0;
			bool success = true;
			isReceiveNotification = false;
			BLEAddress bleAddress(miTHDevices[indexCurrentSensor].address);
			// 尝试三次连接
			Serial.printf("connecting to %s\n", miTHDevices[indexCurrentSensor].address.c_str());
			while (!connectAndRead(bleAddress))
			{
				retryCount++;
				Serial.printf("failed %d times\n", retryCount);
				if (retryCount >= MAX_RETRY)
				{
					success = false;
					Serial.println("all 3 times failed");
					break;
				}
				else
				{
					delay(5000);
				}
			}
			// 判断连接状态
			if (success)
			{
				Serial.println("connected, waiting for notification");
				// 手动阻塞并设置超时 一般传感器5秒发送一次数据 超时设置为20秒
				retryCount = 0;
				while (!isReceiveNotification)
				{
					delay(1000);
					retryCount++;
					// 超时
					if (retryCount >= 20)
					{
						Serial.println("there is no reply from sensor, disconnecting");
						disconnectDevice();
						break;
					}
				}
				// 接收到了notify就发送给mqtt
				if (isReceiveNotification)
				{
					sendMiTHData(indexCurrentSensor);
				}

				if (pClient->isConnected())
				{
					disconnectDevice();
				}
			}
			// 每个传感器之间缓一缓
			delay(2000);
		}
		delay(publishGap);
	}
}

bool connectAndRead(BLEAddress address)
{
	if (pClient->connect(address) == true)
	{
		Serial.printf("signal strength %d \n", pClient->getRssi());

		pRemoteService = pClient->getService(serviceUUID);
		if (pRemoteService == nullptr)
		{
			Serial.print("connection broken: fail to get service");
			if (pClient->isConnected())
				pClient->disconnect();
			return false;
		}
		pRemoteCharacteristic_THB = pRemoteService->getCharacteristic(charUUID);
		if (pRemoteCharacteristic_THB == nullptr)
		{
			Serial.print("connection broken: fail to get characteristic");
			if (pClient->isConnected())
				pClient->disconnect();
			return false;
		}
		else
		{
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

// 收到notification之后的回调
void notifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic_THB, uint8_t *pData, size_t length, bool isNotify)
{
	Serial.println("data received");
	float temp, humi, batt;
	temp = (pData[0] | (pData[1] << 8)) * 0.01;
	humi = pData[2];
	batt = (pData[3] | (pData[4] << 8)) * 0.001;
	Serial.printf("DEVICE %s TEMP %.2f C - HUMI %.2f %% - BATT= %.3f V - free heap= %d\n",
				  miTHDevices[indexCurrentSensor].deviceName.c_str(),
				  temp,
				  humi,
				  batt, esp_get_free_heap_size());
	miTHDevices[indexCurrentSensor].temp = temp;
	miTHDevices[indexCurrentSensor].humi = humi;
	miTHDevices[indexCurrentSensor].batt = batt;
	isReceiveNotification = true;
}

void disconnectDevice()
{
	// 取消notify回调
	pRemoteCharacteristic_THB->registerForNotify(NULL);
	if (pClient != NULL)
		if (pClient->isConnected())
		{
			pClient->disconnect();
			do
			{
				delay(1000);
			} while (pClient->isConnected());
			Serial.println("disconnected");
		}
}