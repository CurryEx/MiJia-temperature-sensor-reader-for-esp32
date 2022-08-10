# 运行在esp32上的米家蓝牙温湿度计2(LYWSD03MMC)mqtt发布器
# A simple MiJia temperature and humidity sensor(LYWSD03MMC) mqtt publisher for esp32.

> **修改并简化自 https://github.com/jaggil/ESP32_Xiaomi-Mijia-LYWSD03MMC** ，感谢作者提供的uuid和esp32 ble使用方法。

> **simplify from https://github.com/jaggil/ESP32_Xiaomi-Mijia-LYWSD03MMC** , thanks so much to the author.

- 功能
    1. 按照指定间隔时间读取温湿度计读数
    2. 通过MQTT发送数据
    3. 可以连接多个设备

- 需要进行的配置
    - 请在文件中搜索todo 配置wifi, mqtt服务器, 时间间隔等参数

- 编译环境
    - PlatformIO or ArduinoIDE
    - pio中安装Espressif 32平台
    - pio中使用esp32cam板型

~~若源码中出现单词拼写错误等纯属正常~~
