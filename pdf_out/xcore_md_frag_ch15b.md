# 《xCore 控制系统使用手册 V2.2_A》第 15 章「RL 指令」整理片段（后半）

> **来源**：`pdf_out/xcore_s15b.txt`（`===== PDF page 281 =====` ~ `===== PDF page 330 =====`，对应手册印刷页 267~316）。
> **覆盖**：15.4.7.5 `SendByte` 的示例片段（p.281）起，至 15.4.19.16 `RMCResetErr`（p.330）止；含 **134 个编号指令条目** + 15.4.14 的 **24 个运算符** + `try/catch` 标准错误码表（-1 / 0 / 101~126）。
> **不含**：15.4.1~15.4.7.4 及 15.4.7.5 的说明与例 1/例 2（在 `xcore_s15a.txt` 中）；任务描述中提到的 Trigger、力控、拖动回放、IO 等更早类别不在本文件；本文件内**未出现 15.5 及之后小节**（以 p.330 的 15.4.19.16 结束）。
> **标注约定**：`（提取不清：…）` 表示原文提取/排版无法确定；`（原文如此）` 表示原文疑似笔误但照录，便于核对。页码均为 PDF 页码（`(p.281)` 形式）。
> **小节索引**：15.4.7 通信（自 15.4.7.5 起）/ 15.4.8 网络 / 15.4.9 逻辑 / 15.4.10 起始点 / 15.4.11 数学 / 15.4.12 位操作 / 15.4.13 字符串操作 / 15.4.14 运算符 / 15.4.15 时钟 / 15.4.16 高级 / 15.4.17 功能 / 15.4.18 寄存器 / 15.4.19 末端工具。

---

### 15.4.7 通信指令

> 本文件从 15.4.7.5 的示例开始；15.4.7 的级标题与 15.4.7.1~15.4.7.5 的正文不在本文件内。

#### 15.4.7.5 SendByte（仅本文件所含例 3、例 4，p.281）

> 语法、参数与例 1、例 2 不在本文件；以下为原文示例片段（“例 4”为原文标注；前一段未显示编号，按顺序推断为“例 3”）。

- 例 3（p.281）：

```text
VAR byte data2[2] = {13,17}
SendByte(data2, "socket0")
```

通过 socket0 发送一个 byte 类型数组变量 data2。会将数组中的内容全部发出去。

- 例 4（p.281）：

```text
VAR byte data2[2] = {13,17,20}
SendByte(data2[2], "socket0")
```

通过 socket0 发送一个 byte 类型变量，该变量为 data2[2]，仅表示数组中的第 2 个元素。此时会将 data2[2] 的值 17 发出去，而不会多发任何其他元素。（原文如此：声明长度为 2 却初始化了 3 个元素）

#### 15.4.7.6 ReadBit（p.281）

**语法**：`Ret = ReadBit(BitNum, TimeOut, name)`

| 项 | 类型 | 说明 |
|---|---|---|
| BitNum | int | 需要读取的 bit 数量，大小应该为 8 的整数倍 |
| TimeOut | int | 超时时间，单位 s，范围 0~86400，默认 60 s |
| name | string | 通信连接 SocketConn 的名称或者串口的名称 |
| Ret | bool 数组 | 接收到的数据；数组第一个元素表示最低位 |

**说明**（原文）：
1）通过网络通信以 TCP 形式接收，外部发送的数据需以 SocketConn 配置好的结束符结尾。
2）通过串口通信方式接收，外部设备只需要发送数据部分即可，无需考虑结束符。

**示例**（原文，p.281）：

```text
bool groupio[16]
groupio = ReadBit(16, 60, "Socket0")
```

示例说明：通过 ReadBit 指令读取 16 个 bit 的数据存储到名为 groupio 的布尔型数组中，超时时间 60 s。假设外部设备发送 ASCII 字符：`95`+结束符，则机器人接收到“95”。其中“9”的十六进制为 0x39，“5”的十六进制为 0x35，因此用户接收到的数据为 0x3935。此时 groupio 数组从 [1]~[16] 表示为：`1001 1100 1010 1100`。[1] 是数据低位，与 0x3935 符合。

**错误码**：本指令可用错误码见 15.4.9.12（110、114、115、119 等）。

#### 15.4.7.7 ReadByte（p.281）

**语法**：`Ret = ReadByte(ByteNum, TimeOut, name)`

| 项 | 类型 | 说明 |
|---|---|---|
| ByteNum | int | 需要读取的 bit 数量，大小应该为 8 的整数倍（原文如此） |
| TimeOut | int | 超时时间，单位 s，范围 0~86400，默认 60 s |
| name | string | 通信连接 SocketConn 的名称或者串口的名称 |
| Ret | byte 数组 | 接收到的数据 |

**说明**：接收一定字节的数据。注意数据之间需要使用逗号隔开。

**示例**（原文，p.281）：

```text
byte rets[6] = {0,0,0,0,0,0}
rets = ReadByte(6,60,"clt1")
```

读取 6 个 byte 的数据存储到名为 rets 的 byte 类型数组中，超时时间 60 s。
注意外部设备的 byte 数据之间需要使用逗号隔开，比如发送 `1,2,3,4,5,6`。

**注意**：通过 TCP 发送数据，需要在数据后面加上预设好的结束符；通过串口发送数据，不需要结束符。

#### 15.4.7.8 ReadDouble（p.282）

**语法**：`Ret = ReadDouble(DoubleNum, TimeOut, name)`

| 项 | 类型 | 说明 |
|---|---|---|
| DoubleNum | double | 需要读取的 double 数个数，最大为 30 个 |
| TimeOut | int | 超时时间，单位 s，范围 0~86400，默认 60 s |
| name | string | 用于接收数据的 Socket 名称 |
| Ret | double 数组 | 接收到的数据 |

**说明**：通过 Socket 接收 double 型数据，外部发送的数据需以配置好的结束符结尾。注意，该指令仅对 TCP 网络通信生效，适用于机器人做客户端和服务端，但不适用于串口。

**示例**（原文，p.282）：

```text
double dd[10]
dd = ReadDouble(10, 60, "Socket0")
```

读取 10 个 double 型的数据存储到名为 dd 的 double 型数组中，超时时间 60 s。

#### 15.4.7.9 ReadInt（p.282）

**语法**：`Ret = ReadInt(IntNum, TimeOut, name)`

| 项 | 类型 | 说明 |
|---|---|---|
| IntNum | int | 需要读取的 int 数个数，最大为 30 个 |
| TimeOut | int | 超时时间，单位 s，范围 0~86400，默认 60 s |
| name | string | 用于接收数据的 Socket 名称 |
| Ret | int 数组 | 接收到的数据（原文：返回值数据类型 int，使用 int 型数组存储接收到的数据） |

**说明**：通过 Socket 接收 int 型数据，外部发送的数据需以配置好的结束符结尾。注意，该指令仅对 TCP 网络通信生效，适用于机器人做客户端和服务端，但不适用于串口。

**示例**（原文，p.282）：

```text
int ii[10]
ii = ReadInt(10, 60, "Socket0")
```

读取 10 个 int 型的数据存储到名为 ii 的 int 型数组中，超时时间 60 s。

#### 15.4.7.10 ReadString（p.282）

**语法**：`Ret = ReadString(TimeOut, name, [len])`

| 项 | 类型 | 说明 |
|---|---|---|
| TimeOut | int | 超时时间，单位 s，范围 0~86400，默认 60 s |
| name | string | 用于接收数据的 Socket 或者串口名称 |
| len | int | 可选参数，使用串口读取时才使用；由于串口中没有加入结束符的概念，因此需要指定长度后才能正确读取和解析 |
| Ret | string | 接收到的字符串 |

**说明**：读取字符串并返回，外部发送的数据需以配置好的结束符结尾。

**示例 1**（原文，网络通信方式，p.282）：

```text
VAR String str1
str1 = ReadString(60, "Socket1")
```

从 Socket1 中接收一个字符串，并存储到 str1 中，超时时间 60 s。

**示例 2**（原文，串口通信方式，p.282）：

```text
VAR String str1
str1 = ReadString(60, "serial0",5)
```

从 serial0 中接收一个长度为 5 的字符串，并存储到 str1 中，超时时间 60 s。

#### 15.4.7.11 GetSocketConn（p.283）

**语法**：`Ret = GetSocketConn(name)`

| 项 | 类型 | 说明 |
|---|---|---|
| name | string | 通信连接 SocketConn 的名称 |
| Ret | SocketConn | 通过字面名称查找到的 socket 属性对象 |

**说明**：从 socket 连接名称查找对应的 socket 属性集对象。该指令获取的结果可以用来进行判断和处理逻辑。仅应该将其当作只读对象使用。该指令仅适用于通信连接（包括机器人作为客户端，或者作为服务端已经连接上的用于通信的通道），不适用于监听服务器和串口。

**可查询属性**（原文，p.283）：

| 可查询属性 | 查询方法 | 含义和示例 |
|---|---|---|
| ip 地址 | ret.ip | 字符串，如 `192.168.0.161` |
| 端口号 | ret.port | 整型，如 8090 |
| 属性 | ret.attr | 机器人做服务器：`incoming`；机器人做客户端：`outgoing`；如果连接未建立：`""` 或者其他值，通常为空 |
| 缓存大小 | ret.cache | 1~100 |
| 名称 | ret.name | 示例为 `client0` |
| 连接状态 | ret.state | closed、establish |

**示例**（原文，p.283）：

```text
SocketConn ret= GetSocketConn("client0")
```

查找名称为 `client0` 的 SocketConn 对象。可以使用 ret 来获得这个连接的属性，包括 ip 地址、端口号、通信结束符、连接状态等信息。

#### 15.4.7.12 GetSocketServer（p.283）

**语法**：`Ret = GetSocketServer(name)`

| 项 | 类型 | 说明 |
|---|---|---|
| name | string | 通信连接 SocketServer 的名称（原文写作 Name） |
| Ret | SocketServer | 通过字面名称查找到的服务器属性对象 |

**说明**：从用户定名称查找对应的服务器属性集对象。该指令获取的结果可以用来进行判断和处理逻辑。仅应该将其当作只读对象使用。该指令仅适用于监听服务器（SocketServer 对象），不适用于通信连接（包括机器人作为客户端，或者作为服务端已经连接上的用于通信的通道）和串口。

**可查询属性**（原文，p.283）：

| 可查询属性 | 查询方法 | 含义和示例 |
|---|---|---|
| ip 地址 | ret.ip | 字符串，如 `192.168.0.161` |
| 端口号 | ret.port | 整型，如 8090 |
| 名称 | ret.name | 示例为 `svr1` |
| 连接状态 | ret.state | closed、listening、error |

**示例**（原文，p.283）：

```text
SocketServer listener1 = {"192.168.0.200", 8090, "svr1"}
OpenDev( "svr1" ) //绑定端口、监听端口
//根据连接标识 "svr1" 获取SocketServer对象，此时ret将复制任务1中listener1的全部状态
SocketServer ret= GetSocketServer("svr1")
if(ret.state == "listening") //使用SocketServer的attr属性，判断是否监听中
//逻辑处理
endif
```

#### 15.4.7.13 GetBufSize（p.284）

**语法**：`Ret = GetBufSize(name)`

| 项 | 类型 | 说明 |
|---|---|---|
| name | string | 串口资源的字面名称 |
| Ret | int | 缓冲区中未处理数据量，以字节为单位 |

**说明**：获取串口缓冲区中还剩余多少数据未读，单位为字节。指令仅适用于串口，不适用于 TCP 服务器和客户端。

**示例**（原文，p.284）：

```text
OpenDev("serial0")
int a = GetBufSize("serial0")
print(a)
```

#### 15.4.7.14 ClearBuffer（p.284）

**语法**：`Ret = ClearBuffer(name)`

| 项 | 类型 | 说明 |
|---|---|---|
| name | string | 串口资源的字面名称 |
| Ret | int | 缓冲区中未处理数据量，以字节为单位（原文如此） |

**说明**：清除缓冲区，未读取完毕的字符将丢失。指令仅适用于串口，不适用于 TCP 服务器和客户端。

#### 15.4.7.15 ReadOpcUaVarByName（p.284）

**语法**：`ReadOpcUaVarByName(name, value)`

| 项 | 类型 | 说明 |
|---|---|---|
| name | string | OPC-UA 自定义变量名称 |
| value | bool/byte/int/double/string | 用于存放读取到的 OPC-UA 自定义变量的值；如果 value 的类型与 opcua 变量的类型不一致，将自动转换。注意：string 转换为数值类型时，都为 0 |

**说明**：通过名称读取 OPC-UA 自定义变量的值。无返回值。

**示例**（原文，p.284）：

```text
int value = 0
ReadOpcUaVarByName("int_var", value)
print(value)
```

#### 15.4.7.16 WriteOpcUaVarByName（p.284）

**语法**：`WriteOpcUaVarByName(name, value)`

| 项 | 类型 | 说明 |
|---|---|---|
| name | string | OPC-UA 自定义变量名称 |
| value | bool/byte/int/double/string | OPC-UA 变量的修改值；类型不一致将自动转换。注意：string 转换为数值类型时，都为 0 |

**说明**：通过名称修改 OPC-UA 自定义变量的值。

**示例**（原文，p.284）：

```text
int value = 0
WriteOpcUaVarByName("int_var", value)
WriteOpcUaVarByName("int_var", 123)
```

---

### 15.4.8 网络指令

> 本节 8 条指令**全部被标注为“过期的”**：为 xCore 控制系统 1.3 版本使用指令，在更高版本中依然有效，但不再继续维护，也不推荐继续使用。（原文，各条一致，p.285~288）

#### 15.4.8.1 SocketCreate（过期的，p.285）

**语法**：`SocketCreate（"ip_Address", Port, "Name", [Cache] [, "Terminator"]）`

| 项 | 类型 | 说明 |
|---|---|---|
| ip_Address | string | 定义需要连接 server 的 ipv4 地址，需使用双引号包含 |
| Port | int | 定义 server 端口号 |
| Name | string | 定义新建 Socket 的名称，不同 Socket 之间需指定不同的名称 |
| Cache | int | 定义 Socket 缓存大小，通信数据存在缓存队列中，可省略 |
| Terminator | string | 定义 Socket 通信的结束符类型，可省略，默认是 `\r` |

**返回值**：bool，创建成功返回 true，创建失败返回 false。

**说明**：建立一个 Socket 连接，通过使用 Socket 指令，RL 程序可以从外部设备获取数据或者向外发送程序数据。RL 语言支持同时建立多个不同的 Socket 以便于连接多个外部设备，不同 Socket 之间采用不同的名称来进行区分。Socket 指令是基于 TCP/IP 协议的，因此理论上任何支持 TCP/IP 的外部设备都可以和 RL 程序通信以交换数据。所有发送给 RL Socket 指令的数据（即使用 SocketRead 系列指令接收的数据），都应该以“回车”结尾，在接收到“回车”之前的所有数据都将合并做为同一条数据处理。使用 Socket 功能时，机器人控制器仅支持作为 client 连接外部 server。最多支持创建 10 个 Socket 连接。

**示例**（原文，p.285）：

```text
if (SocketCreate("10.0.6.11",8080,"S1",10,"\r"))
// 创建成功
else
// 错误处理
endif
```

**注意**：由于 TCP/IP 协议资源释放机制限制，请不要频繁调用 SocketCreate 和 SocketClose 指令，否则可能会造成程序运行出错。为避免循环模式下频繁调用 SocketCreate 和 SocketClose 指令，两条指令之间最好增加时间延时，例如：

```text
SocketClose("S1")
wait 0.1
SocketCreate("10.0.6.11",8080,"S1",10,"\r")
```

#### 15.4.8.2 SocketClose（过期的，p.285）

**语法**：`SocketClose（"SocketName"）`

| 项 | 类型 | 说明 |
|---|---|---|
| SocketName | string | 需要关闭的 Socket 名称 |

**说明**：关闭 Socket。

**示例**（原文，p.285）：`SocketClose（"Socket0"）`

**注意**：不要在 SocketSend 系列指令后直接使用 SocketClose 指令，否则可能造成数据发送失败，等待收到确认消息后再使用 SocketClose 指令。

#### 15.4.8.3 SocketSendString（过期的，p.286）

**语法**：`SocketSendString（StringData，"SocketName"）`

| 项 | 类型 | 说明 |
|---|---|---|
| StringData | string | 待发送的 string 数据 |
| SocketName | string | 用于发送数据的 Socket 名称 |

**说明**：通过 Socket 对外发送一个字符串。

**示例 1**（原文）：`SocketSendString（"Hello World"，"Socket0"）` —— 通过 Socket0 对外发送 Hello World 字符串。

**示例 2**（原文）：

```text
VAR String str1 ="Hello World"
SocketSendString（str1，"Socket0"）
```

通过 Socket0 发送 str1 存储的字符串。

#### 15.4.8.4 SocketSendByte（过期的，p.286）

**语法**：`SocketSendByte(ByteData, "SocketName")`

| 项 | 类型 | 说明 |
|---|---|---|
| ByteData | int 或 byte 或 byte 数组 | 发送一个 0~255 的无符号字节或数组，主要用于发送 ASCII 码 |
| SocketName | string | 用于发送数据的 Socket 名称 |

**说明**：通过 Socket 对外发送一个字节 byte，在需要发送 ASCII 字符时非常有用。

**示例**（原文，p.286）：

```text
例 1
SocketSendByte(13, "socket0")        //通过 socket0 对外发送一个回车符

例 2
VAR byte data1 = 13
SocketSendByte(data1, "socket0")     //先定义 byte 变量 data1（回车符），再通过 socket0 对外发送

例 3
VAR byte data2[2] = {13,17}
SocketSendByte(data2, "socket0")     //通过 socket0 发送一个 byte 类型数组变量 data2
```

#### 15.4.8.5 SocketReadBit（过期的，p.286~287）

**语法**：`SocketReadBit(BitNum, TimeOut, "SocketName")`

| 项 | 类型 | 说明 |
|---|---|---|
| BitNum | int | 需要读取的 bit 数量，大小应该为 8 的整数倍 |
| TimeOut | int | 超时时间，单位 s，范围 0~86400，默认 60 s |
| SocketName | string | 用于接收数据的 Socket 名称 |
| 返回值 | bool（原文如此；正文说使用 bool 型数组存储接收到的 bit 数据，每个 bit 对应一个 bool 成员） | 接收到的 bit 数据 |

**说明**：通过 Socket 按 bit 接收数据，外部发送的数据需以回车结尾。

**示例**（原文，p.287）：

```text
bool groupio[16]
groupio = SocketReadBit(16, 60, "Socket0")
```

通过 SocketReadBit 指令读取 16 个 bit 的数据存储到名为 groupio 的布尔型数组中，超时时间 60 s。

#### 15.4.8.6 SocketReadDouble（过期的，p.287）

**语法**：`SocketReadDouble(DoubleNum, TimeOut, "SocketName")`

| 项 | 类型 | 说明 |
|---|---|---|
| DoubleNum | double | 需要读取的 double 数个数，最大为 30 个 |
| TimeOut | int | 超时时间，单位 s，范围 0~86400，默认 60 s |
| SocketName | string | 用于接收数据的 Socket 名称 |
| 返回值 | double 数组 | 接收到的数据 |

**说明**：通过 Socket 接收 double 型数据，外部发送的数据需以回车结尾。

**示例**（原文，p.287）：

```text
double dd[10]
dd = SocketReadDouble(10, 60, "Socket0")
```

#### 15.4.8.7 SocketReadInt（过期的，p.287）

**语法**：`SocketReadInt(IntNum, TimeOut, "SocketName")`

| 项 | 类型 | 说明 |
|---|---|---|
| IntNum | int | 需要读取的 int 数个数，最大为 30 个 |
| TimeOut | int | 超时时间，单位 s，范围 0~86400，默认 60 s |
| SocketName | string | 用于接收数据的 Socket 名称 |
| 返回值 | int 数组 | 接收到的数据 |

**说明**：通过 Socket 接收 int 型数据，外部发送的数据需以回车结尾。

**示例**（原文，p.287）：

```text
int ii[10]
ii = SocketReadInt(10, 60, "Socket0")
```

#### 15.4.8.8 SocketReadString（过期的，p.287~288）

**语法**：`SocketReadString(TimeOut, "SocketName")`

| 项 | 类型 | 说明 |
|---|---|---|
| TimeOut | int | 超时时间，单位 s，范围 0~86400，默认 60 s |
| SocketName | string | 用于接收数据的 Socket 名称 |
| 返回值 | string | 存储接收到的字符串 |

**说明**：从 Socket 读取一个字符串并返回，外部发送的数据应以回车结尾。

**示例**（原文，p.288）：

```text
VAR String str1
str1 = SocketReadString(60, "Socket1")
```

从 Socket1 中接收一个字符串，并存储到 str1 中，超时时间 60 s。

---

### 15.4.9 逻辑指令

| 指令 | 语法原型 | 说明 | 页码 |
|---|---|---|---|
| Return | （无参数） | 函数返回：程序在子函数中时返回到上一级函数；在主函数中时程序直接结束 | (p.288) |
| Wait | `Wait <时间>` | 程序等待一段时间，范围 0~2147484 秒；例 `Wait 2` 表示等待 2 s | (p.288) |
| WaitUntil | `WaitUntil(cond,\MaxTime,\TimeFlag)`（原文如此，参数前带反斜杠） | 程序等待某个条件成立，若超时则将超时标志置 true 并结束等待继续向下执行；cond：bool 逻辑表达式；MaxTime：可选，超时等待时间，单位 s，int 或 double；TimeFlag：可选，bool 超时标志位 | (p.288~289) |
| Break | （WHILE 循环内） | 跳出当前循环；在 WHILE 中执行到 Break 时，不管 CONDITION 如何都直接跳出 WHILE 循环 | (p.289) |
| IF…Else if…Else | `IF(condition1) … Else if (condition2) … Else … Endif` | 条件判断语句；condition1 成立执行逻辑 a，condition2 成立执行逻辑 b，以此类推 | (p.289) |
| Goto | `Goto <标记>` | Goto 语句允许把指针跳转到被标记的语句（示例中标记写作 `end:`） | (p.289) |
| For | `For(int i from 1 to 10) … endfor`；`For(int i from 1 to 10 step 3) … Endfor` | For 循环允许编写一个执行指定次数的循环控制结构；Continue 和 Break 可用来控制 For 的流程 | (p.289~290) |
| Continue | （循环内） | 跳出本次循环，继续从循环起始处执行下条语句，但不退出循环体，仅仅结束本次循环 | (p.290) |
| Inzone | `Inzone … EndInzone` | 与 SetDO 或者 modbus、cclink 等 IO 操作或指令配合使用，可保证信号在确定的点位触发，不会被前瞻指针提前触发；补充说明见下 | (p.290~291) |
| While | `while(cond) … endwhile` | While 循环允许编写一个在条件满足前不断执行的循环控制结构；Continue 和 Break 可用来控制 While 的流程 | (p.291) |
| Pause | `Pause` | 暂停程序运行；程序会在 pause 语句的前一句执行完毕后进入暂停状态，必须使用示教器点击运行或者通过外部程序启动信号才可恢复程序运行 | (p.291) |
| try/catch | `try … catch(error e) … endtry` | RL 语言的错误处理机制；try 到 catch 之间的指令出错后，程序会将执行错误转换为错误信息集合 `e` 并从 catch–endtry 的代码块继续运行；详细见下 | (p.291~294) |
| SwitchCase | `Switch(condition) … Case C1,C12,C13: … Case C2: … Default: … EndSwitch` | 类似 IF 的流程控制：将 condition 依次与 Case 字段变量比较，相等则进入对应分支且不再进行后续比较；所有条件不满足则进入 Default；没有 Case 匹配且没有 Default 时不进入任何分支，Switch 结束；Case 可输入多个条件 | (p.294) |

**WaitUntil 原文示例**（p.288~289）：

```text
例1
WaitUntil (di2 == true)           //等待 di2 信号值为 true，然后才开始执行后面的语句

例2
WaitUntil (di2 == true,5)
//等待 di2 为 true；若等待超过 5s 仍为 false，则执行后面的语句

例3
Bool flag = false
WaitUntil (di2 == true, 5,flag)
//等待超过 5s 仍为 false，则将 flag 置为 true 后继续；
//若在 5s 内 di2 变为 true，则 flag 置为 false。可将 flag 用于后续的逻辑判断
```

**Break 原文示例**（p.289）：

```text
VAR int counter = 0
WHILE(1)
IF(counter == 5)
break
Endif
counter++
ENDWHILE
```

该程序在执行到 counter 等于 5 时会跳出 WHILE 循环。

**IF…Else if…Else 原文示例**（p.289）：

```text
IF(condition1)
//a
Else if (condition2)
//b
Else if (condition3)
//c
Else
//d
Endif
```

**Goto 原文示例**（p.289）：

```text
int a = 0
int b = 9
Goto end
print(a)
end:
print(b)
```

先定义两个变量 a 和 b，然后用 print 函数打印两句话，直接用 Goto 语句强制跳转到打印 b 语句的 end 标记位置，此时 a 的打印就不会执行了。

**For 原文示例**（p.290）：

```text
例 1
For(int i from 1 to 10)
printf("i = %d\n", i)
endfor
//该程序把 i 从 1 到 9 每次加 1 依次打印 9 次（原文如此）

例 2
For(int i from 1 to 10 step 3)
printf("i = %d\n", i)
Endfor
//该程序把 i 从 1 到 10 每次加 3 依次打印 3 次
```

**Continue 原文示例**（p.290）：

```text
VAR int count = 0
WHILE(1)
count++
IF(count == 1)
Continue
Else
break
MoveAbsJ j10， v500， fine， tool1
Endif
ENDWHILE
```

MoveAbsJ 的代码将不会被执行到。

**Inzone 原文示例与补充说明**（p.290~291）：

```text
MoveL p1
MoveL p2
Inzone
SetDO dox, true
print(123)
EndInzone
MoveL p3
```

补充说明：在示例中，使用了一个 Inzone 指令，解释器前瞻到 Inzone 之后，并不会立即执行，而是生成了一个附加函数，函数内容是 SetDO 以及 print 指令，这个附加函数会在运动指令 move p2 完成之后生效。
1、如果 p2 p3 两条运动指令之间存在转弯区，则附加函数会在机器人进入两段运动的转弯区的时刻开始执行；
2、如果没有转弯区，则附加函数会在机器人到达 p2 的时刻开始执行。

**While 原文示例**（p.291）：

```text
int count = 0
while(count < 10)
count++
print(count)
endwhile
```

该程序实现一个 count 从 0 到 10 每次加 1 并打印的循环。

**Pause 注意**（p.291）：该指令暂时不支持辅助编程。

#### 15.4.9.12 try/catch 详解与标准错误码（p.291~294）

**语法**：

```text
try
// do something
catch(error e)
print(e)
endtry
```

举例：从网络链接读数据是很有可能失败的指令，但是此时不希望机器人停机，可以用 try-catch 将错误捕获并通过 RL 编程处理。

**error 类型定义**（p.291）：error 是一个结构体，一共有四个参数组成：

| 成员 | 类型 | 含义 |
|---|---|---|
| file | string | 错误发生文件名 |
| line | int | 错误行 |
| num | int | 错误码 |
| reason | string | 错误原因 |

error 结构体可以通过 print 指令直接打印：

```text
// error data
...
catch(error e)
print(e.file)
print(e.line)
print(e.num)
print(e.reason)
print(e)
endtry
```

**原文示例 1**（p.292）：

```text
ReadOnce:
Try
Double xyz[3] = ReadDouble(3, timeout, socketname)
Robtarget_0.trans.x = xyz[1]
Robtarget_0.trans.y = xyz[2]
Robtarget_0.trans.z = xyz[3]
MoveL Robtarget_0, v2000, fine, tool0
Catch(error e)
SendString("Recv rob xyz error", socketname)
Goto ReadOnce
endtry
```

该程序实现了一个简单的应用场景，使用通信指令 ReadDouble 从 TcpSocket 读取一个三维数组作为运动点位的 xyz 参数，然后使用 MoveL 指令运动到对应笛卡尔点。如果没有使用 try/catch 并且从 TcpSocket 收到的点位是错误数据，则机器人会报错“超出运动范围”或者“规划错误”，并且停止程序的运行。如果使用了 try/catch，虽然依然会报告运动指令错误，但是程序不会停止，而是跳转到 catch 到 endtry 的代码段，执行用户想要的错误处理（本样例中通过 SendString 告诉 Socket 上位机收到的点位错误，再由上位机决定如何处理，并执行 goto 重新执行 ReadDouble 等待下一次的位置）。

**原文示例 2**（p.292）：

```text
re_read:
try
opendev("conn_name")
string_res = readstring("conn_name")
catch (error e)
if (e.num == xxx)
// 某种可处理的错误不暂停
goto re_read
else
print(e)
Pause
endif
endtry
```

**注意**（p.292）：力控指令不能触发 try-catch。

**try/catch 能够处理的错误类型及标准错误码**（p.292~294）：

> 该表跨 3 页且含合并单元格，原文提取顺序有交错；下表按列语义归并，数值区间原文用句点者（如 `(0. 86400]`）照录。

| 分类 | 出错指令 | 说明 | error.num | error.reason |
|---|---|---|---|---|
| 默认错误 | 未分配专属错误码的指令 | 未分配专属错误码的指令 | -1 | 未知错误 |
| 串口相关指令 | （原文未单列） | 串口不存在时 | -1 | 未知错误 |
| 运动相关指令 | MoveXX、Search、TrigL 等 | 运动坐标的工具、工件错误 / 运动速度错误 / 运动负载错误 | -1 | 未知错误 |
| 运动相关指令 | AccRamp、HomeSet 等运动参数设置 | 超出运动范围 / 规划错误 / 遇到奇异点等 | -1 | 未知错误 |
| 网络指令 | OpenDev | 网络链接的端口错误 | -1 | 未知错误 |
| 网络指令 | 所有网络指令 | 通过 RL 操作外部通讯的连接 | -1 | 未知错误 |
| 计算、逻辑指令 | CalcJoinT、CalcRobt、CRobT、CJointT、CLKSTOP、GOTO | 控制器内部错误 | -1 | 未知错误 |
| 外设控制（Jodell 系列）（RM 系列） | JodellGripInit、JodellSuckInit、JodellSuckStatus、RMRGMGripPosMove、RMRGMGripTrqMove、RMRGMGripStatus、RMRGMResetErr、RMCGripPosMove、RMCGripTrqMove、RMCGripStatus、RMCResetErr、RMRGMGripInit、RMCGripInit | 外设通讯异常 | -1 | 未知错误 |
| 激光控制 | Laser 所有指令 | 激光焊接已关闭 | -1 | 未知错误 |
| 码垛控制 | TrayUpdate、TrayCount、PalletUpdate、PalletLayerCount、PalletWobjCount、SolarVisionExec | 与上位机数据收发错误 | -1 | 未知错误 |
| 寄存器控制 | ReadRegByteByName | 读取数据失败 | -1 | 未知错误 |
| 四轴锁定 | SingAreaLockAxis4 | 位姿错误，无法开启四轴锁定功能 | -1 | 未知错误 |
| 解释器内部错误 | 绝大部分指令 | 参数类型、数量错误 | -1 | XXX 参数错误 |
| 解释器内部错误 | （原文未单列） | （提取不清：原文此行仅给出 `0`，无说明与原因文字） | 0 | （提取不清） |
| 网络指令 / 串口指令 | OpenDev | 连接到服务器失败 | 101 | OpenDevConn失败 |
| 网络指令 / 串口指令 | OpenDev | 机器人作为服务端开启失败 | 102 | OpenDevServer失败 |
| 网络指令 / 串口指令 | GetSocketConn | SocketConn 对应的连接未建立 | 103 | GetSocketConn失败 连接不存在 |
| 网络指令 / 串口指令 | GetSocketConn | 获取 SocketConn 结构体的名字是一个服务器 SocketServer | 104 | GetSocketConn 失败 对象是服务器 |
| 网络指令 / 串口指令 | GetSocketServer | 获取 SocketServer 结构体的名字错误 | 105 | GetSocketServer失败 服务器不存在 |
| 网络指令 / 串口指令 | OpenDev | 开启连接输入参数错误，变量列表无对应连接 | 106 | OpenDev失败 使用不存在的对象 |
| 网络指令 / 串口指令 | SocketAccept | 输入参数不是服务器名称（SocketAccept(server) 指令需要服务器名称） | 107 | SocketAccept 指令需要服务器名称 |
| 网络指令 / 串口指令 | GetSocketConn | 获取 SocketConn 结构体的名字错误 | 108 | GetSocketConn(conn) 不存在的 SocketConn |
| 网络指令 / 串口指令 | GetSocketServer | 获取 SocketConn 结构体的名字错误 | 109 | GetSocketConn(server) 不存在的 SocketServer |
| 网络指令 / 串口指令 | ReadBit | 指令输入参数错误 | 110 | ReadBit 必须读取8的整数倍数 |
| 网络指令 / 串口指令 | ReadDouble | 指令输入参数错误 | 111 | ReadDouble 指令超出预设范围(0, 4096] |
| 网络指令 / 串口指令 | ReadInt | 指令输入参数错误 | 112 | ReadInt 指令超出预设范围(0, 4096] |
| 网络指令 / 串口指令 | ReadByte | 指令输入参数错误 | 113 | ReadByte 指令超出预设范围(0, 4096] |
| 网络指令 / 串口指令 | ReadBit、ReadDouble、ReadInt、ReadByte、ReadString | 输入的时间太长 | 114 | ReadXX 指令时间超出预设范围(0. 86400] |
| 网络指令 / 串口指令 | ReadBit、ReadDouble、ReadInt、ReadByte、ReadString | 连接断开 或者 读取数据错误 | 115 | Read指令失败 |
| 网络指令 / 串口指令 | ReadDouble | 超出限定时间 | 116 | ReadDouble 指令超时 |
| 网络指令 / 串口指令 | ReadInt | 超出限定时间 | 117 | ReadInt 指令超时 |
| 网络指令 / 串口指令 | ReadString | 超出限定时间 | 118 | ReadString 指令超时 |
| 网络指令 / 串口指令 | ReadBit | 超出限定时间 | 119 | ReadBit 指令超时 |
| 网络指令 / 串口指令 | ReadByte | 超出限定时间 | 120 | ReadByte 指令超时 |
| 网络指令 / 串口指令 | SendString | 指令超时或者连接断开 | 121 | SendString 指令超时或者连接断开 |
| 网络指令 / 串口指令 | SendByte | 指令超时或者连接断开 | 122 | SendByte 指令超时或者连接断开 |
| 传送带跟踪指令 | WaitObj | 执行指令时，工件已经越过启动窗口，无法跟踪 | 123 | Out StartWindow |
| 传送带跟踪指令 | WaitObj | 等待跟踪工件超时 | 124 | Out WaitTime |
| 传送带跟踪指令 | WaitObj | 重复跟踪工件 | 125 | Connected Twice |
| 传送带跟踪指令 | WaitObj | 开启跟踪后可能发生 跟踪过程超出工作区域抛出异常 | 126 | Out MaxDistance |

#### 15.4.9.13 SwitchCase（p.294~295）

**定义结构**（原文）：

```text
Switch(condition)
Case C1,C12,C13:
Functions1()
Case C2:
Functions2()
Default:
DefaultFunction()
EndSwitch
```

**说明**：SwitchCase 指令跟 IF 指令类似，是根据输入的变量条件进行流程控制的指令。RL 解释器将根据输入的变量（condition）依次与 Case 字段的变量进行比较。如果两个变量相等，解释器将进入对应 Case 的代码分支，并且不再进行后续比较，不会进入其他代码分支；如果所有条件都不满足，则会进入 Default 分支；如果没有 Case 条件匹配且没有 Default 分支，则不会进入任何分支，Switch 指令结束；Case 指令可以输入多个条件（见指令结构 `Case C1,C12,C13` 和示例 1）。

**示例 1**（原文，p.295）：

```text
Switch(reg_int)
Case 1,2,3:
FunctionsA() // 机器人走功能A相关点位
Case 4,5,6:
FunctionsB() // 机器人走功能B相关点位
Default:
FunctionC() // 无指定输入执行C功能
EndSwitch
```

示例背景：reg_int 是一个寄存器变量，上位机（PLC）会通过相关寄存器协议（如 modbus、cclink）更新变量的数值，生产工程希望机器人根据寄存器的数值执行对应的函数分支（比如走不同的运动轨迹），如果寄存器输入 1,2,3 则执行 A 函数，如果输入 4,5,6 则执行 B 函数，如果不符合上述条件，则进入 Default 分支执行 C 函数。

---

### 15.4.10 起始点指令

| 指令 | 语法原型 | 说明 | 页码 |
|---|---|---|---|
| Home | `Home`（指令无参数） | 以轴空间运动让机器人回到设定好的初始点 | (p.295) |
| HomeSet | `HomeSet axis1,axis2,axis3,axis4,axis5,axis6,axis7` | 设定机器人的轴空间初始点位置；Axisx 数据类型 double，设定在初始点各个轴的角度 | (p.295) |
| HomeSetAt | `HomeSetAt(index)` | 获得机器人的初始点的设定数据；返回 double，关节角，单位°；Index 为 int，index 为 0 时返回是否开启了 Home 设置（1 表示已开启，0 表示未开启） | (p.295~296) |
| HomeDef | `HomeDef()` | 判断是否设置了初始点；返回 bool：true 已设置初始点，false 未设置初始点 | (p.296) |
| HomeSpeed | `HomeSpeed Speed` | 设定 Home 指令的运行速度 | (p.296) |
| HomeClr | `HomeClr` | 清除初始点设置 | (p.296) |

**原文示例**（p.295~296）：

```text
例 1（Home / HomeSet）
HomeSet 0,30,0,60,0,90,0
Home
//通过 HomeSet 指令设置初始点，再通过 Home 指令让机器人运动到轴空间的拖拽位姿

例（HomeSetAt）
HomeSet 0,30,0,60,0,90,0
double angle2 = HomeSetAt(2)
//angle2 获得关节2的关节角 30°

例（HomeSpeed）
HomeSpeed v1000
Home
//设定初始点运动的速度为 V1000，随后 Home 指令让机器人按照 V1000 的速度往初始点运动

例（HomeClr）
HomeClr
//清除程序中设定的起始点。清除后 Home 指令将无法执行
```

**注意**（p.295）：必须在机器人设置>快速调整界面开启 Home 位姿设置或者通过 HomeSet 指令开启 Home 位姿设置，才可以使用 Home 指令，否则 Home 指令会报错。

---

### 15.4.11 数学指令

| 指令 | 语法原型 | 说明 | 页码 |
|---|---|---|---|
| Sin | `double sin(double x)` | 计算参数 x 的正弦值；x 单位为弧度；返回 -1 至 1 之间的计算结果 | (p.296) |
| Cos | `double cos(double x)` | 计算参数 x 的余弦值；x 单位为弧度；返回 -1 至 1 之间的计算结果 | (p.296) |
| Tan | `double tan(double x)` | 计算参数 x 的正切值；x 单位为弧度；返回参数 x 的正切值 | (p.296) |
| Cot | `double cot(double x)` | 计算参数 x 的余切值；x 单位为弧度；返回参数 x 的余切值 | (p.297) |
| Asin | `double asin(double x)` | 反正弦；x 范围为 -1 至 1 之间，超过此范围则会报错；返回 -PI/2 至 PI/2 之间的计算结果，单位为弧度 | (p.297) |
| Acos | `double acos(double x)` | 反余弦；x 范围为 -1 至 1 之间，超过此范围则会报错；返回 0 至 PI 之间的计算结果，单位为弧度 | (p.297) |
| Atan | `double atan(double x)` | 反正切；返回 -PI/2 至 PI/2 之间的计算结果 | (p.297) |
| Sinh | `double sinh(double x)` | 双曲线正弦；数学定义式为 `(exp(x)-exp(-x))/2`；返回参数 x 的双曲线正弦值 | (p.297) |
| Cosh | `double cosh(double x)` | 双曲线余弦；数学定义式为 `(exp(x)+exp(x))/2`（原文如此）；返回参数 x 的双曲线余弦值 | (p.297) |
| Tanh | `double tanh(double x)` | 双曲线正切；数学定义式为 `sinh(x)/cosh(x)`；返回参数 x 的双曲线正切值 | (p.297) |
| Exp | `double exp(double x)` | 计算以 e 为底的 x 次方值（e^x）；返回 e 的 x 次方计算结果 | (p.298) |
| Ln | `double ln(double x)` | 计算以 e 为底的 x 对数值（自然对数 ln(x)，x > 0）；返回参数 x 的自然对数值 | (p.298) |
| log10 | `double log10(double x)` | 计算以 10 为底的 x 对数值；要求 x>0；返回参数 x 以 10 为底的对数值 | (p.298) |
| pow | `double pow(double x, double y)` | 计算以 x 为底的 y 次方值（x^y）；返回 x 的 y 次方计算结果 | (p.298) |
| sqrt | `double sqrt(double x)` | 计算参数 x 的平方根；参数 x 必须为正数；返回参数 x 的平方根值 | (p.298) |
| ceil | `double ceil(double x)` | 返回不小于参数 x 的最小整数值，结果以 double 形态返回 | (p.298) |
| floor | `double floor(double x)` | 返回不大于参数 x 的最大整数值，结果以 double 形态返回 | (p.298) |
| abs | `int abs(int x)` / `double abs(double x)` | 求 x 的绝对值 \|x\|；输入 int 输出 int，输入 double 输出 double | (p.298~299) |
| rand | `rand()` | 产生一个整型随机数；返回一个整型随机数，范围为 0~2147483647 | (p.299) |

---

### 15.4.12 位操作

| 指令 | 语法原型 | 说明（含原文示例结果） | 页码 |
|---|---|---|---|
| BitAnd | `BitAnd (BitData1, BitData2)` | 针对 byte 类型数据生成逻辑位与操作；返回 byte。例：data1=34、data2=38，`BitAnd(data1, data2)` 得 34 | (p.299) |
| BitCheck | `BitCheck (BitData, BitPos)` | 检查 byte 数据某一位是否为 1，为 1 返回 true，否则 false；BitPos 范围 1~8。例：data1=130，`BitCheck(data1, 8)` 返回 true | (p.299) |
| BitClear | `BitClear BitData \| IntData, BitPos` | 将 byte 或 int 类型数据的某一位置为 0，位数从 1 开始；BitPos 对 byte 为 1-8、对 int 为 1-32。例：data1=255，`BitClear data1 1` 得 254，`BitClear data1 2` 得 252 | (p.300) |
| BitLSh | `BitLSh (BitData, ShiftSteps)` | 对 byte 数据执行逻辑左移；返回 byte；ShiftSteps 范围 1~8。例：data1=38 左移 3 位得 48 | (p.300) |
| BitNeg | `BitNeg (BitData)` | 对 byte 数据执行逻辑非操作；返回 byte。例：data1=38，`BitNeg(data1)` 得 217 | (p.300) |
| BitOr | `BitOr (BitData1, BitData2)` | 对 byte 数据执行逻辑或操作；返回 byte。例：data1=39、data2=162，`BitOr(data1, data2)` 得 167 | (p.300~301) |
| BitRSh | `BitLSh (BitData, ShiftSteps)`（原文如此，定义行写作 BitLSh） | 对 byte 数据执行逻辑右移；返回 byte；ShiftSteps 范围 1~8。例：data1=38 右移 3 位得 4 | (p.301) |
| BitSet | `BitSet BitData \| IntData, BitPos` | 将 byte 或 int 类型数据的某一位置为 1，位数从 1 开始；BitPos 对 byte 为 1-8、对 int 为 1-32。例：data1=0，`BitSet data1 1` 得 1，`BitSet data1 2` 得 3（原文说明句写作“赋值 255”，与示例 `= 0` 不符——原文如此） | (p.301) |
| BitXOr | `BitXOr (BitData1, BitData2)` | 对 byte 数据执行逻辑异或操作；返回 byte。例：data1=39、data2=162，得 133（原文示例代码行写作 `BitOr(data1, data2) //133`——原文如此） | (p.301~302) |

---

### 15.4.13 字符串操作

| 指令 | 语法原型 | 说明 | 页码 |
|---|---|---|---|
| StrFind | `StrFind (Str ChPos Set [\NotInSet])`（原文参数间无逗号） | 在字符串中查找，从特定位置开始查找属于另一个特定字符集合的位置；返回 int：第一个字符匹配的位置；没找到返回字符串长度+1；ChPos 从 1 开始，越界报错；`[\NotInSet]` 标识搜索不在匹配字符集合内的字符。例：`StrFind("Robotics", 1, "aeiou")` 得 2；加 `\NotInSet` 得 1 | (p.302) |
| StrLen | `StrLen (Str)` | 获取字符串长度；返回 int，>=0。例：`StrLen("Robotics")` 得 8 | (p.302) |
| StrMap | `StrMap (Str, FromMap, ToMap)` | 创造一份 string 备份，所有字符按指定映射关系替换，映射字符根据位置一一对应，没有映射的字符保持不变；返回替换得到的字符串。例：`StrMap("Robotics", "aeiou", "AEIOU")` 得 `RObOtIcs`；使用限制：FromMap 和 ToMap 必须匹配、长度一致 | (p.302~303) |
| StrMatch | `StrMatch (Str, ChPos, Pattern)` | 从指定位置开始搜索特定格式或字符串，返回匹配字符串首字符所在位置；没有匹配则返回字符串长度+1；ChPos 超出字符串长度范围报错。例：`StrMatch("Robotics", 1, "bo")` 得 3 | (p.303) |
| StrMemb | `StrMemb (Str, ChPos, Set)` | 检查字符串中某个字符是否属于指定的字符集合；返回 bool；ChPos 超出字符串范围报错。例：`StrMemb("Robotics", 2, "aeiou")` 返回 true | (p.303) |
| StrOrder | `StrOrder (Str1, Str2)` | 比较两个字符串并返回布尔值；str1<=str2 时返回 true，否则 false。例：`StrOrder("FIRST", "SECOND")` 为 true；`StrOrder("FIRSTB", "FIRST")` 为 false | (p.303) |
| StrPart | `StrPart (Str, ChPos, Len)` | 截取字符串一部分生成新字符串，返回从指定位置开始截取指定长度的字符串；ChPos 超出字符串范围时报错。例：`StrPart("Robotics", 1, 5)` 得 `Robot` | (p.303~304) |
| StrSplit | `StrSplit (Str [, separator])` | 通过指定分隔符把字符串分割成字符串数组；返回 string 数组；Separator 中所有字符都被看做分隔符，可以缺省，缺省时空格作为默认分隔符。例：`string str_arr[4] = StrSplit("test1,test2;test3\test4", "\,;")` 分割为四个子串（test1 test2 test3 test4）。使用限制：输入字符串为空时会报错；分割结果和定义字符串的长度不匹配时会报错 | (p.304) |
| StrToByte | `StrToByte (Str, [ trans])` | 将字符串转换为 byte 类型数据；Trans 为枚举：`\Bin`（二进制）、`\Okt`（八进制）、`\Hex`（十六进制）、`\Char`（字符）以及默认参数（无参数，十进制）。例：`StrToByte("10", \Bin)`=2、`\Okt`=8、默认=10、`\Hex`=16；`StrToByte("0", \Char)`=48。使用限制：输入字符串不符合指定的数据格式时会报错 | (p.304~305) |
| StrToDouble | `StrToDouble (Str)` | 将字符串转换为 double 类型数据；返回字符串的转换结果。例：`StrToDouble("3.1415926")`。使用限制：输入字符串不符合指定的数据格式时会报错 | (p.305) |
| StrToInt | `StrToDouble (Str)`（原文如此，定义行写作 StrToDouble；正文说明为“将字符串转换为 Int 类型数据”） | 将字符串转换为 Int 类型数据。例：`Int NumInt = StrToInt("99")`。使用限制：输入字符串不符合指定的数据格式时会报错 | (p.305) |
| StrToDoubleArray | `StrToDoubleArray(output, input, spilit)` | 对大量 double 字符串进行类型转换，转换成 double 数组数据；返回值类型 Int，转换是否遇到异常：-1 错误，0 正常；Output 为 double 数组（转换结果输出）；Input 为 str 字符串（输入的字符串）；Spilit 为 str 字符串（字符串的分隔符）。例：`string tmp_ss = "1,2,3,4,5,6,7"`、`double db_arr[10]`、`StrToDoubleArray (db_arr, tmp_ss, ",")`，结果 `db_arr = {1,2,3,4,5,6,7,0,0,0}`。使用限制：字符串允许结尾出现一个多余的结束符；转换失败或含非法字符报错“输入的字符串分割后不是 Double 形式”；double 数组应大于等于字符串中数据量的大小，否则报错“输入的数组大小不足 或者 数组不是一维数组” | (p.305) |

---

### 15.4.14 运算符

#### 15.4.14.1 基础运算符

##### 15.4.14.1.1 算数运算符（p.306）

| 运算符 | 作用 |
|---|---|
| + | 加 |
| - | 减/负号 |
| * | 乘 |
| / | 除 |
| % | 取余 |
| -- | 自减 |
| ++ | 自加 |

说明（原文）：算数运算操作符支持 bool、byte、int、double 类型数据的操作；不同类型的变量如果放在一起进行加减乘除运算，会触发隐式转换。

原文示例：

```text
例 1
VAR int a = 1
VAR int b = 2
VAR int c = -b //取负
VAR int ac = a * c //乘法

例 2
++，--两个运算符又称单目运算符，是指对一个操作数进行操作的运算符，RL不区分前后自加自减：
x = n++ //表示n先加1，再将n的值赋给x
x = --n //表示n先减1后，再将新值赋给x
```

隐式转化结果（原文，p.306）：

| 类型1 | 类型2 | 结果 |
|---|---|---|
| bool | bool | bool |
| bool | byte | byte |
| bool | int | int |
| bool | double | double |
| byte | byte | byte |
| byte | int | int |
| byte | double | double |
| int | int | int |
| int | double | double |
| double | double | double |

##### 15.4.14.1.2 逻辑运算符（p.307）

逻辑运算符支持基本数据类型的运算，包括：

| 运算符 | 作用 |
|---|---|
| && | 逻辑与 |
| \|\| | 逻辑或 |
| < | 小于 |
| > | 大于 |
| <= | 小于等于 |
| >= | 大于等于 |
| == | 等于 |
| != | 不等于 |
| ! | 取逻辑非 |

说明：逻辑与 && 表达式为真的条件是两边的结果都为真，而逻辑或 \|\| 为真的条件是两边只要有一个条件为真即可。

原文示例：

```text
VAR int res = 1
while(res < 3) //比较res是否小于3
res++
endwhile
di5 = !di6 //取逻辑非
VAR int counter = 4
while(di7 && di8) //求逻辑与
if(counter == 5) //是否相等
break
endif
endwhile
```

##### 15.4.14.1.3 赋值运算符（p.307~308）

| 运算符 | 作用 |
|---|---|
| = | 赋值 |
| += | 加等 |
| -= | 减等 |
| *= | 乘等 |
| /= | 除等 |
| %= | 取模等 |

原文示例：

```text
VAR int num1 = 3
VAR int num2 = 4
num1 += num2 //等同于 num1 = num1 + num2 则num1 = 7。
num1 -= num2 //等同于 num1 = num1 – num2，则num1 = -1。
num1 *= num2 //等同于 num1 = num1 * num2 则num1 = 12。
num1 /= num2 //等同于 num1 = num1 / num2 则num1 = 0。
num1 %= num2 //等同于 num1 = num1 % num2 则num1 = 3。
```

隐式转换说明（原文）：变量所有赋值操作，都支持隐式转换。当赋值运算左右两边的数据类型不一致时，解释器会尝试触发隐式转换以使得程序能继续运行，当转换失败时，程序会报错停止。Bool、Byte、Int、Double 四种数据类型之间能进行转换。IO、寄存器变量属于上面四种变量的特殊形式，如果它们用于赋值操作，也可触发隐式转换。函数的返回值如果属于上面四种类型，也可作为赋值操作的右值，进行赋值计算。

原文示例：

```text
例1
int tmp_num = 10.5 // 10
bool tmp_bool = 1 // true
tmp_bool = 0 // false
double tmp_d = 999 // 999.0

例2
// 直接用寄存器变量修改普通变量
double tmp_num = register0
// 寄存器变量可以直接用于条件判断
WaitUntil(register0 == 10)

例3
int mem_ret = StrMemb("Robotics", 2, "aeiou")
// StrMemb的返回值是 bool 类型，如果一定要用int类型接收返回值，
// 控制器不会报错，而是会进行隐式转换
// true -> 1 ， false-> 0
```

##### 15.4.14.1.4 其他运算符（p.308~309）

| 运算符 | 作用 |
|---|---|
| （） | 圆括号 |
| . | 点操作符 |

原文示例：

```text
例 1
VAR int num = arr[1] //取数组第一个元素赋给num
VAR int num2 = (1+2)*3 //使用括号可以改变运算顺序，这里num2的值为9

例 2
定义一个robtarget变量pt1
pt1.trans.x = 200 //使用“.”操作符将pt1点的x坐标改为200
```

使用限制：“.”操作符不支持对 robtarget 变量 A，B，C 成员的修改。

#### 15.4.14.2 运算优先级（p.309）

| 优先级 | 运算符 | 使用形式 | 结合方向 |
|---|---|---|---|
| 1 | ( ) | （表达式）/函数名(形参表) | — |
| 1 | . | 变量名. | — |
| 2 | - | -表达式 | 右到左 |
| 2 | ++ | ++变量名/变量名++ | — |
| 2 | -- | --变量名/变量名-- | — |
| 2 | ! | !表达式 | — |
| 3 | / | 表达式/表达式 | 左到右 |
| 3 | * | 表达式*表达式 | — |
| 3 | % | 整型表达式/整型表达式 | — |
| 4 | + | 表达式+表达式 | 左到右 |
| 4 | - | 表达式-表达式 | — |
| 5 | > | 表达式>表达式 | 左到右 |
| 5 | >= | 表达式>=表达式 | — |
| 5 | < | 表达式<表达式 | — |
| 5 | <= | 表达式<=表达式 | — |
| 6 | == | 表达式==表达式 | 左到右 |
| 6 | != | 表达式!= 表达式 | — |
| 7 | && | 表达式&&表达式 | 左到右 |
| 8 | \|\| | 表达式\|\|表达式 | 左到右 |
| 9 | = | 变量=表达式 | 右到左 |
| 9 | /= | 变量/=表达式 | — |
| 9 | *= | 变量*=表达式 | — |
| 9 | %= | 变量%=表达式 | — |
| 9 | += | 变量+=表达式 | — |
| 9 | -= | 变量-=表达式 | — |

> 注：原文优先级 5、6、9 的多行共用同一“结合方向/优先级”单元格，上表按语义展开重复。

---

### 15.4.15 时钟指令

| 指令 | 语法原型 | 说明 | 页码 |
|---|---|---|---|
| ClkRead | `ClkRead (Clock)` | 读取计时器的值；返回 double，返回计时器停止时刻或当前时刻距启动 clock 的时间间隔，精度 0.001 s；Clock 数据类型 clock，计时器名称 | (p.309) |
| ClkReset | `ClkReset Clock` | 重置一个计时器；使用一个计时器前可通过 ClkReset 保证计数为 0 | (p.310) |
| ClkStart | `ClkStart Clock` | 启动一个计时器；计时器启动后它会不断运行计数，直到计时器停止或程序重置；即使程序停止或机器人下电，计时器仍会继续运行 | (p.310) |
| ClkStop | `ClkStop Clock` | 停止一个计时器；计时器停止后停止计数，可被读取间隔、重新启动或重置 | (p.310) |

原文示例（p.309~310）：

```text
VAR clock clock1
ClkStart clock1
ClkStop clock1
VAR double interval=ClkRead(clock1)
//interval 存储 clock1 在启动和停止之间的时间间隔
```

---

### 15.4.16 高级指令

| 指令 | 语法原型 | 一句话说明 | 页码 |
|---|---|---|---|
| RelTool | `RelTool(Point, XOffset, YOffset, ZOffset, Rx, Ry, Rz [, Tool, Wobj])` | 在当前指令指定的工具坐标系下对空间位置进行平移或者旋转；返回 robtarget（偏移后的新位姿）。与 Offs 主要区别：Offs 是相对于工件坐标系偏移，RelTool 是相对于工具坐标系偏移 | (p.310~311) |
| Offs | `Offs(Point, XOffset, YOffset, ZOffset [, Rx, Ry, Rz])` | 把某个点在当前指令中指定的工件坐标系下偏移一段距离并返回新点位置；平移偏移量由 x、y、z 表示，姿态旋转偏移量由 Rx、Ry、Rz 表示；返回 robtarget | (p.311) |
| ConfL | `ConfL Off` / `ConfL On` | 关闭/打开 conf 限制：Off 时解除 conf 限制，让控制器尝试计算出离路径起点最（较）近的逆解（可能算不出来，导致运动指令失败）；On 时打开 conf 检查 | (p.312) |
| VelSet | `VelSet gain` | 调节最大运动速度能力；Gain 为 int，按百分比指定，取值范围 1%~100%，其中 100% 对应最大加速度（原文），超过限制范围机器人会报错 | (p.312~313) |
| AccSet | `AccSet acc, ramp` | 调节加速度/加加速度；Acc、Ramp 均为 int，按系统预设值百分比指定，取值范围均为 30%~100%，100% 对应最大值，超过限制范围机器人会报错 | (p.313) |
| MotionSup | `MotionSup type [, level, event]` | 打开/关闭碰撞检测；Type 关键字 On/Off；Level 为 int，碰撞检测灵敏度百分比，范围 [1,200]；Event 为 string，设置碰撞后的行为：`"safestop"` 安全停止、`"pause"` 触发暂停（只有协作机器人支持，工业机器人不支持）、`"softstop"` 柔顺停止 | (p.313) |
| MotionSupPlus | `MotionSupPlus x1,x2,x3,x4,x5,x6,x7` | 在 RL 程序中随时调整机器人各关节碰撞检测灵敏度参数；x1~x7 分别表示关节 1 到关节 7 的碰撞检测灵敏度，单位 % | (p.313~314) |
| BreakLookAhead | （指令无参数和返回值） | 通知控制系统取消前瞻，强制取消上一条运动指令和下一条运动指令之间的转弯区；机器人 TCP 运行至上一条运动指令的目标点后再向下一个点位运动，不衔接转弯区；程序指针也将等待 TCP 运行至上一条运动指令的目标点后再继续往下前瞻扫描 | (p.314) |
| GetRobotMaxLoad | `Ret = GetRobotMaxLoad()` | 获取当前型号机器人的最大负载值；Ret 为 int，最大负载 | (p.314) |
| GetRobotState | `Ret = GetRobotState()` | 获取控制系统当前的运行状态；用 4 个字节的 bit 信息表示控制系统状态，包括故障、紧急停止、安全门、操作模式、伺服模式、运动状态等；Ret 为 byte 数组（四个 byte） | (p.314~315) |
| AutoIgnoreZone | `AutoIgnoreZone true/false` | 指定是否允许控制系统自动忽略转弯区；true：允许（控制系统默认状态）；false：不允许 | (p.315~316) |
| MotionWaitAtFinePoint | `MotionWaitAtFinePoint true/false` | 设定当前瞻遇到 fine 点时机器人是否立即启动运动（Fine 点＝没有衔接转弯区的目标点，即转弯区参数写作 fine 的目标点）；控制系统默认状态为 false | (p.316~317) |
| DynamicThreshold | `DynamicThreshold mark,switch` | 将对应程序“囊括”进动态阈值区间，区间内碰撞检测自动辨识并使用最适合当前区间的阈值；Mark 为 int，用于标记不同动态阈值区间，避免阈值混淆；Switch 为 bool，true/false 分别代表动态阈值区间的开启和关闭 | (p.317) |
| DynamicThresholdPause | `DynamicThresholdPause type` | 在程序块前后插入，暂停使用动态阈值、转用预设固定阈值；Type 为关键字，on/false 分别代表暂停使用动态阈值功能开启和关闭（原文如此） | (p.317~318) |
| IgnoreOverride | `IgnoreOverride On/Off` | 暂时屏蔽速率滑块对运动指令的影响；On 表示其后运动指令不受速率滑块影响，Off 效果相反；支持自动模式和手动模式 | (p.318~319) |
| SingAreaLockAxis4 | `SingAreaLockAxis4 on/off` | 采用锁定 4 轴的方式规避机器人手腕奇异点；On 开启，Off 关闭 | (p.319) |
| SpeedRefresh | `SpeedRefresh override` | 覆盖当前运动程序任务中的速度值；Override 为 int，取值范围 1%~100% 速度覆盖值，超过限值范围机器人会报错 | (p.319) |
| CSpeedOverride | `Ret = CSpeedOverride()` | 用户读取当前速度覆盖值；Ret 为 int，取值范围 1~100% 速度覆盖值 | (p.319~320) |
| SingAreaJointWay | `SingAreaJointWay on/off,zone` | 采用关节空间轨迹插补的方式规避笛卡尔指令中的奇异点；on/off 分别代表关节插补奇异功能的开启和关闭；Zone 表示切割后三段轨迹间的转弯区半径 | (p.320) |
| SingAreaWrist | `SingAreaWrist on/off, limit` | 采用牺牲姿态的方式规避笛卡尔指令中的奇异点；on/off 分别代表牺牲姿态规避奇异点功能开启和关闭；Limit 表示设置的允许最大的可牺牲姿态的值，单位角度 | (p.320~321) |

**原文参数与要点摘录**（p.310~321）

- RelTool：Point/XOffset/YOffset/ZOffset/Rx/Ry/Rz 均为 double（Point 为 robtarget），Tool/Wobj 为可选；未指定工具工件时默认使用 tool0、wobj0；RelTool 与 Move 指令配合使用、未指定特定工具工件坐标系时，将使用运动指令的 tool 和 wobj。该指令的可选参数 Tool 和 Wobj 暂时不支持辅助编程。注意：xMate CR 系列的 5 轴机型由于一个姿态自由度的缺失，在使用 RelTool 命令时会有较多点位不可达情况，需要结合可达姿态特点确定偏移（如在法兰平行基座的情况下点位的平移通常都可达）。
- Offs：Point/XOffset/YOffset/ZOffset/Rx/Ry/Rz 均为 double（Point 为 robtarget）；该指令暂时不支持辅助编程。示例：`p11=Offs(p10,100,200,300)` 将 p10 沿工件坐标系 x 偏移 100 mm、y 偏移 200 mm、z 偏移 300 mm，赋给 p11。
- ConfL：笛卡尔坐标对应一组 conf 参数（cf1~7, cfx），用户手动更改或写入的笛卡尔坐标点对应的 conf 数据可能是错误的，导致控制器无法解析出目标点的路径；部分场景用户只关心机器人 TCP 点的位置而不关心姿态，此时可用 ConfL Off。注意：HMI 点位列表界面的“运动至”功能默认打开 conf 限制（即 ConfL On）；RL 编程默认关闭 conf 限制（即 ConfL Off）。
- VelSet 注意：1. VelSet 指令只影响所在的 RL 工程的运动指令，对非工程的 JOG 功能、运动至功能、快速移动功能等均不生效。2. VelSet 功能会打断转弯区，请勿在需要转弯区的运动指令之间插入 VelSet 指令。3. VelSet 指令和程序运行速率调整滑块的功能区别：速率滑块修改的是用户期望速度（如 V4000 在 50% 滑块下相当于 V2000 的用户期望速度），但若机器人处于极限工况、运动指令能达到的实际最大速度只有 V1000，则滑块 50% 或 100% 都不改变实际速度；VelSet 50 不改变用户期望速度，而是在运动规划过程中将该运动指令能达到的实际最大速度缩小 50%（上例中实际速度从 V1000 变为 V500）。4. 发生以下操作时速度自动恢复为默认大小（100%）：RL 程序被手动重置时（PP to Main）、加载新的 RL 程序时。
- AccSet 注意：发生以下操作时加速度自动回复为默认大小（100%）：RL 程序被手动重置时（PP to Main）、加载新的 RL 程序时。示例：`AccSet 50,15` 加速度、加加速度设置为默认的一半。
- MotionSupPlus 注意：当用于六轴机器人，也需要设置 7 个参数，其中前 6 个参数起作用，对应 1~6 关节；该指令支持协作机器人和六轴工业机器人，不支持三、四轴工业机器人。
- BreakLookAhead：1）若 P1 的转弯区设置为 z50，由于该指令存在前瞻将取消、转弯区也将取消，机器人 TCP 会精确运动至 P1 点再向 P2 运动；而 P2 和 P3 之间没有该指令，因此在 P2 点会前瞻并保留 z50 转弯区向 P3 运动。2）BreakLookAhead 指令与 `wait 0` 指令效果完全相同。
- GetRobotMaxLoad 示例：`int maxload = GetRobotMaxLoad()`、`print(maxload)`；以 xMate 7 为例，返回值为 7。
- GetRobotState 示例：`byte st[4] = GetRobotMaxLoad()`（原文如此，示例行调用的是 GetRobotMaxLoad）、`print(st)`；返回 {0,5,0,0} 时，查表可知当前状态为：无故障，电机已上电，自动模式，机器人运动状态，伺服处于位置模式。状态位表见下。
- AutoIgnoreZone：机器人运行两条 MoveL 指令、中间含 z50 转弯区时，控制系统为运动柔顺性和安全性需向前前瞻一段距离；当前瞻终点 p1 和转弯区起点 p2 重合时，若已收到第二条运动指令便可正常生成转弯区；若未收到，则依据 AutoIgnoreZone 状态处理：true 时控制系统不会等待第二条运动指令，而是将转弯区取消，直接向着 P3 点运动；false 时控制系统会等待第二条运动指令，在此过程中机器人会降低运动速度，直至生成转弯区轨迹；若机器人运动到 p2 点时仍未收到第二条运动指令，则机器人会停止运动，并通过 HMI 报错。未能及时收到第二条运动指令往往在于两条运动指令间加入了过多的非运动指令（例如非常多的打印指令）。
- MotionWaitAtFinePoint：true 时控制系统严格按照前瞻参数控制机器人启动，当前瞻距离达到前瞻参数设定值或前瞻完所有运动指令时机器人才启动运动，此状态下能够保证设定的前瞻距离长度；false 时控制系统不严格遵守前瞻参数，当前瞻遇到 fine 点时机器人立即启动运动，此状态下遇到非常复杂的程序逻辑机器人依然能够顺利启动，但无法保证前瞻距离的长度。
- DynamicThreshold 注意：程序循环运行到动态阈值区间的前三次用于辨识，并不会进行碰撞检测，请保证辨识时无外界干扰。示例：mark 对应不上的两条指令之间的运动不会使用动态阈值（示例 2 中 `DynamicThreshold 2,true` 与 `DynamicThreshold 23,false` mark 对应不上，运动到 p5 的过程不会使用动态阈值）。
- DynamicThresholdPause：该指令需与 DynamicThreshold 指令配合使用。
- IgnoreOverride：焊接、涂胶等对沿路径的运动速度有严格要求的场景，希望工艺段的运动速度不受全局速率影响；该指令支持在自动模式和手动模式下使用。手动模式下运动速度受 v250 限制，速度超过 v250 按照 v250 执行；自动模式下则按期望速度运动。受影响的运动指令：MoveAbsJ、MoveJ、MoveL、MoveC、MoveCF、MoveT、SearchL、SearchC、TrigL、TrigC、TrigJ。注意：该指令未立即执行指令，不打断转弯区；只能在运动任务中使用且不能在 Inzone 中使用，否则报错。
- SingAreaLockAxis4：只有当机器人已经 4 轴处于 0 度或者 ±180 度时才能开启该功能（通常 on 前一句运动指令的目标点位需要保证 4 轴处于上述点位，或者机器人四轴已经处于上述角度），否则将会引发报错提示。注意：on 和 off 之间的笛卡尔运动指令，其姿态将采用特殊的插补方式，不会改变 4 轴的运动角度，任何试图改变 4 轴角度的运动指令都会引起报错；该指令被设计为阻塞指令，会打断前后运动指令间的转弯区。目前版本适用机型为：工业标准六轴系列（XB,NB 型号）和协作 xMateCR（5 轴机型除外）、xMateSR 系列。
- SpeedRefresh：RL 程序被手动重置（PP to Main）或重新加载 RL 程序、重新运行、下一步时，通过 SpeedRefresh 指令设置的速度覆盖值将会被重置，重新使用示教器界面上设置的程序速度。注意：通过 SpeedRefresh 设置的速度覆盖值不会立即完成，下达命令与对物理机械臂产生速度影响之间会存在一定时间滞后。
- CSpeedOverride 示例：`int override = CSpeedOverride()`、`print(override)`；若当前速度覆盖值为 70%，则返回 70。
- SingAreaJointWay：on 和 off 之间的笛卡尔运动指令，控制系统会自动检测其上是否有奇异点：不含奇异点按普通轨迹运动，含奇异点则按该模式特有方式运动——围绕奇异点 Psingular 在原轨迹上加入 Pcut1 和 Pcut2 两点，将原轨迹划分为 P0Pcut1、Pcut1Pcut2、Pcut2P1 三部分，其中 P0Pcut1 和 Pcut2P1 依然为原笛卡尔轨迹，Pcut1Pcut2 采用关节空间轨迹（MoveAbsJ）代替原轨迹部分，从而能够通过奇异点；三段轨迹间采用转弯区平滑过渡，转弯半径由 zone 参数设置。机器人在奇异点附近的运动关节角度通常运动幅度很大，使用时需要确认机器人的运动轨迹是否符合要求。注意：目前版本适用机型为工业标准六轴系列（XB,NB 型号）。
- SingAreaWrist：on 和 off 之间的笛卡尔运动指令都使用牺牲姿态的方式来运动，机器人工具遵循正确的、精准的轨迹运动，但机器人腕关节形态将被改变（未通过奇异点时亦将出现上述情况）；腕关节姿态可能运动幅度很大，使用该指令时需要确认机器人的运动轨迹是否符合要求。目前版本适用机型包括工业标准六轴系列（XB,NB 型号）和协作 xMateCR、xMateSR 系列。注意：只可以对直线运动使用，不可对弧线运动使用；使用牺牲姿态奇异规避功能，示教点在奇异点范围内时，示教点将被变更，运动时的手腕姿态与示教的可能有所不同（不仅通过奇异点的示教点，而且以后的示教点的姿态也可能被改变）；单步运动时和连续运动时的奇异点附近姿态的运动可能不同。示例中 p1 与 p2、p2 与 p3 之间的转弯区可正常生成，p3 和 p4 之间的转弯区不可生成。

**GetRobotState 状态位表**（原文，p.314~315）：

| 序号 | 状态位 | 含义 |
|---|---|---|
| 1 | Byte[1].bit[1] | 1：控制系统未授权 |
| 2 | Byte[1].bit[2] | 1：控制系统可恢复故障 |
| 3 | Byte[1].bit[3] | 1：控制系统致命错误 |
| 4 | Byte[1].bit[4] | 1：伺服系统故障 |
| 5 | Byte[1].bit[5] | 1：伺服系统致命故障 |
| 6 | Byte[1].bit[6] | 1：紧急停止 |
| 7 | Byte[1].bit[7] | 1：安全门停止 |
| 8 | Byte[1].bit[8] | 保留 |
| 9 | Byte[2].bit[1] | 上电状态，0：电机未上电；1：电机已上电 |
| 10 | Byte[2].bit[2] | 机器人运动状态，0：空闲；1：运动中 |
| 11 | Byte[2].bit[3] | 操作模式，0：手动模式；1：自动模式 |
| 12 | Byte[2].bit[4] | 伺服模式，0：位置模式；1：力矩模式 |
| 13 | Byte[2].bit[5] | 保留 |
| 14 | Byte[2].bit[6] | 保留 |
| 15 | Byte[2].bit[7] | 保留 |
| 16 | Byte[2].bit[8] | 保留 |
| 17 | Byte[3] | 保留 |
| 18 | Byte[4] | 保留 |

---

### 15.4.17 功能指令

| 指令 | 语法原型 | 说明 | 页码 |
|---|---|---|---|
| CRobT | `CRobT（ Tool， Wobj）` | 获取机器人位姿；需要给定工具名称和工件名称，返回指定工具坐标系的 pose、当前的轴配置信息以及外部轴位置；返回值 robtarget | (p.321) |
| CJointT | `CJointT（）` | 读取机器人轴和外部轴当前角度；返回 jointtarget，旋转轴单位：度，线性轴单位：mm | (p.322) |
| CalcJointT | `CalcJointT（Rob_Target，Tool，Wobj）` | 根据指定的 robtarget 变量计算对应的关节角度；返回 jointtarget（关节角度单位 °，直线外部轴单位 mm，旋转外部轴单位 Degree） | (p.322) |
| CalcRobt | `CalcRobt（Joint_Target，Tool，Wobj）` | 根据指定的关节角度计算对应的笛卡尔空间位姿；返回 robtarget | (p.322) |
| Print | `Print（ var1， var2，……）` | 将用户定义的内容打印输出到示教器，可用于程序调试；输入参数个数不限，但至少要有一个，且每个参数必须为一个定义过的变量或者常量；系统将变量转换为字符串并串接，输出到程序编辑器的调试窗口（调试窗口关闭时 print 的信息仍被记录，记录数量上限为 500 条） | (p.322~323) |
| PoseMult | `pose3 = PoseMult(pose1，pose2)` | 计算两个位姿变换的乘积；pose1、pose2、pose3 均为 pose | (p.323) |
| PoseInv | `pose2 = PoseInv(pose1)` | 计算一个位姿变换的逆；pose1 为输入位姿，pose2 为返回值 | (p.323) |
| GetRobABC | `double db_arr[3] = GetRobABC(Point [, A, B, C])` | 获取笛卡尔空间点 P 的欧拉角姿态 ABC；返回 double 三维数组；可选输出参数 A、B、C 为 double | (p.324) |
| SetRobABC | `SetRobABC(Point , A, B, C)` | 根据输入的欧拉角 ABC 设置笛卡尔空间点 P 的姿态；A、B、C 为 double，单位°（度） | (p.324) |
| RotRobABC | `RotRobABC(Point , A, B, C)` | 根据输入的欧拉角 ABC，在笛卡尔空间 P 已有姿态的前提下再进行欧拉角旋转；表现为已有的欧拉角跟输入的 ABC 相加；A、B、C 单位°（度） | (p.324~325) |
| OpMode | `ret = OpMode()` | 获取机器人当前的运行模式；ret 为 int：0 为未定义模式，1 为自动模式，2 为手动模式 | (p.325) |

**原文要点摘录**（p.321~325）

- CRobT 注意：使用 CRobT 时，机器人应处于停止状态，即 CRobT 之前的运动语句转弯区设置应为 fine。示例：`p2 = CRobT( tool1， wobj2 )`。
- CJointT 注意：使用 CJointT 时，机器人应处于停止状态，即 CRobT 之前的运动语句转弯区设置应为 fine（原文如此，写的是 CRobT）。示例：`VAR jointtarget j2`、`j2 = CJointT（）`。
- CalcJointT：Rob_Target 为指定的笛卡尔空间目标点，请注意该点定义时使用的工具和工件应和 CalcJointT 指令中使用的工具/工件保持一致，否则可能会导致错误的结果；Tool/Wobj 需与定义该 robtarget 时使用的工具/工件一致。示例：`jpos2 = CalcJointT(pt1, tool1,wobj2)` —— 计算 tool1 到达 pt1 点时对应的关节角度，并赋值给 jpos2，pt1 点是在工件 wobj2 下定义的。
- CalcRobt：Joint_Target 为给定的用来计算笛卡尔空间位姿的关节角度；Tool/Wobj 为计算时使用的工具/工件。示例：`pt1 = CalcRobT（ jpos1, tool2,wobj1）` —— pt1 是工具坐标系 tool2 在工件坐标系 wobj1 下描述的位姿。
- Print 注意：当需要输出字符串时可以使用双引号 `""` 来包含想要显示的字符，但不支持在双引号中嵌套双引号。示例：

```text
counter = 0
while(true)
counter++
Print("counter = ",counter)
endwhile
```

- GetRobABC：旋转顺序定义为，初始坐标系（运动指令中选定的工件坐标系）首先绕自身 X 轴旋转后，再绕初始坐标系的 Y 轴旋转，最后绕初始坐标系的 Z 轴旋转。示例 1：

```text
VAR double Rob_A
VAR double Rob_B
VAR double Rob_C
// 将point0的欧拉角赋值到Rob_A|B|C
GetRobABC(point0, Rob_A, Rob_B, Rob_C)
```

示例 2：`double db_arr[3] = GetRobAbc(point0)`。
- SetRobABC 示例：`SetRobABC(point0, 30, 60, 90)` —— point0 是一个笛卡尔点位变量，点位绕定轴 X,Y,Z 轴依次旋转 30°，60°，90°。
- RotRobABC 示例：`RotRobABC(point0, 30, 60, 90)` —— point0 是一个笛卡尔点位变量，将点位绕 X,Y,Z 再旋为 30°, 60°, 90°（原文如此）。
- OpMode 示例：`int mode = OpMode()`、`print(mode)`；若当前为自动模式则返回 1，手动模式则返回 2。

---

### 15.4.18 寄存器指令

> 寄存器变量来自「设置>通信>寄存器」界面功能（原文）。以下 4 条指令参数与示例照录。

#### 15.4.18.1 ReadRegByName（p.325）

**语法**：`ReadRegByName(RegData，Value)`

| 项 | 类型 | 说明 |
|---|---|---|
| RegData | 可读的寄存器变量 | 设置>通信>寄存器界面功能，寄存器变量 |
| Value | bool/int/double | 寄存器数据将写入 Value；如果寄存器的变量类型和解释器变量不一致，将自动进行格式转换 |

**说明**：根据寄存器的名字，读取对应寄存器的变量值。

**示例**（原文）：

```text
int tmp_int
ReadRegByName(modbus_int_read[6], tmp_int)
```

将名为 modbus_int_read 的下标为 6 的数据，读取到 tmp_int 变量中。

#### 15.4.18.2 WriteRegByName（p.325）

**语法**：`WriteRegByName(RegData，Value)`

| 项 | 类型 | 说明 |
|---|---|---|
| RegData | 可写的寄存器变量 | 设置>通信>寄存器界面功能，寄存器变量 |
| Value | bool/int/double | 将 Value 数据写入寄存器中；如果寄存器的变量类型和解释器变量不一致，将自动进行格式转换 |

**说明**：根据寄存器的名字，读取对应寄存器的变量值（原文如此）；该指令如果放在运动指令之后使用，不会打断转弯区，在运动指令轨迹终点触发，或在转弯区起点触发，具体使用可参考 SetDO 例 2。

**示例**（原文）：

```text
WriteRegByName(modbus_int_write[6], 200)
```

将 INT 200 的数据，写入 modbus_int_write[6] 对应的寄存器中。

#### 15.4.18.3 ReadRegByteByName（p.325~326）

**语法**：`ReadRegByteByName(RegData，Value，byteFlag)`

| 项 | 类型 | 说明 |
|---|---|---|
| RegData | 可读或可写的 int16/int32 类型寄存器 | 通信>寄存器界面功能，寄存器变量 |
| Value | byte 类型变量 | 读取寄存器对应字节值到 Value 中，Value 必须为 byte 类型变量 |
| byteFlag | 字节标志位 | 取值范围 1-4，1 为低字节，4 为高字节 |

**说明**：根据寄存器的名字，读取寄存器对应字节的值。

**示例**（原文）：

```text
byte tmp_value
ReadRegByteByName(modbus_reg, tmp_value,1)
```

将名为 modbus_reg 寄存器的第一个字节，读取到 tmp_value 变量中。

#### 15.4.18.4 WriteRegByteByName（p.326）

**语法**：`WriteRegByteByName(RegData，Value，byteFlag)`

| 项 | 类型 | 说明 |
|---|---|---|
| RegData | 可写的 int16/int32 类型寄存器变量 | 通信>寄存器界面功能，寄存器变量 |
| Value | byte | 将 Value 数据写入寄存器对应字节中 |
| byteFlag | 字节标志位 | 取值范围 1-4，1 为低字节，4 为高字节 |

**说明**：根据寄存器的名字，写对应寄存器字节的值；该指令如果放在运动指令之后使用，不会打断转弯区，在运动指令轨迹终点触发，或在转弯区起点触发，具体使用可参考 SetDO 例 2。

**示例**（原文）：

```text
WriteRegByteByName(modbus_reg, 200,2)
```

将 200 的数据，写入 modbus_reg 的第 2 个字节中。

---

### 15.4.19 末端工具指令

> 本类共 16 条：钧舵电爪/吸盘（Jodell 系列）与增广电爪（RMRGM/RMC 系列）。**原文各条“示例”栏均为空**，以下照录语法与参数。

#### 15.4.19.1 JodellGripInit（p.326）

**语法**：`JodellGripInit ID,wait_time`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 建立通信，初始化钧舵电爪，参数 ID |
| Wait_time | Int 变量 | 等待初始化完成，等待时间阈值，超时报错；单位：s |

**说明**：钧舵电爪的初始化指令。

#### 15.4.19.2 JodellGripMove（p.326）

**语法**：`JodellGripMove ID,Pos,Vel,Trq`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制电爪运动的电爪 ID |
| Pos | Int 变量 | 目标位置，无单位，设置范围 0-255 |
| Vel | Int 变量 | 电爪运行的速度，无单位，设置范围 0-255 |
| Trq | Int 变量 | 电爪运行检测的力，无单位，设置范围 0-255 |

**说明**：钧舵电爪的运动指令。

#### 15.4.19.3 JodellGripStatus（p.326~327）

**语法**：`JodellGripStatus ID,Pos,Vel,Trq,Contact`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 获取电爪运动状态的电爪 ID |
| Pos | Int 变量 | 获取电爪当前位置，无单位，数据范围 0-255 |
| Vel | Int 变量 | 获取电爪运行的速度，无单位，数据范围 0-255 |
| Trq | Int 变量 | 获取电爪运行的力矩，无单位，数据范围 0-255 |
| Contact | Int 变量 | 获取电爪运行的状态字，无单位，数据范围 0-255，其中 bit6-7 表示电爪是否检测到物体的状态 |

**说明**：获取钧舵电爪的状态。

**Contact 状态字含义**（原文，p.327）：

| Bit | 名称 | 值/说明 |
|---|---|---|
| 0 | gAct | 0 表示电动夹爪复位中，1 表示电动夹爪处于使能状态 |
| 2 | gMode | 0 表示处于参数控制模式，1 表示处于无参数控制模式 |
| 3 | gGTO | 0 表示停止，1 表示正在向目标位置运动 |
| 4-5 | gSTA | 0 电动夹爪处于复位或巡检状态，1 表示正在激活，2 表示未使用，3 表示激活完成 |
| 6-7 | gOBJ | 0 表示手指正在指定位置移动；1 表示手指在张开达到指定位置时，由于接触到物体停止；2 表示手指在闭合达到指定位置时，由于接触到物体停止；3 手指到达指定的位置，但没有检测到对象 |

#### 15.4.19.4 JodellSuckInit（p.327）

**语法**：`JodellSuckInit ID`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 初始化此 ID 的吸盘，检测是否正确连接此 ID 的吸盘 |

**说明**：钧舵吸盘的初始化指令。

#### 15.4.19.5 JodellSuckSet（p.327）

**语法**：`JodellSuckSet ID,CH1_enable,CH1_VacMin, CH1_VacMax, CH1_Waittime, CH2_enable, CH2_VacMin, CH2_VacMax, CH2_Waittime`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制吸盘工作的吸盘 ID |
| CH1_enable | Int 变量 | 吸盘第一个通道是否工作，1 表示工作，0 表示不工作 |
| CH1_VacMin | Int 变量 | 吸盘第一个通道最小真空度设置值，范围 0-255，0 表示纯真空，大于 100 表示吸盘释放；当实际真空度低于此阈值时停止抽气 |
| CH1_VacMax | Int 变量 | 吸盘第一个通道最大真空度设置值，范围 0-255，0 表示纯真空，大于 100 表示吸盘释放；当实际真空度高于此阈值时开始抽气 |
| CH1_Waittime | Double 变量 | 吸盘第一个通道超时设置值 |
| CH2_enable | Int 变量 | 吸盘第二个通道是否工作，1 表示工作，0 表示不工作 |
| CH2_VacMin | Int 变量 | 吸盘第二个通道最小真空度设置值，范围 0-255，0 表示纯真空，大于 100 表示吸盘释放；当实际真空度低于此阈值时停止抽气 |
| CH2_VacMax | Int 变量 | 吸盘第二个通道最大真空度设置值，范围 0-255，0 表示纯真空，大于 100 表示吸盘释放；当实际真空度高于此阈值时开始抽气 |
| CH2_Waittime | Double 变量 | 吸盘第二个通道超时设置值 |

**说明**：钧舵吸盘工作的指令，下发此指令后，吸盘立刻按照设定参数开始工作。

#### 15.4.19.6 JodellSuckStatus（p.327~328）

**语法**：`JodellSuckStatus ID,Vac1,Contact1,Time_Err1,Vac2,Contact2,Time_Err2`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 获取吸盘状态的吸盘 ID |
| Vac1 | Int 变量 | 获取的吸盘通道一当前的实际真空度，数据范围 0-100 |
| Contact1 | Int 变量 | 获取的吸盘通道一当前的状态信息数据，数据范围 0-255，其中 bit6-7 表示是否检测到物体 |
| Time_Err1 | Int 变量 | 获取的吸盘通道一是否超时报警 |
| Vac2 | Int 变量 | 获取的吸盘通道二当前的实际真空度，数据范围 0-100 |
| Contact2 | Int 变量 | 获取的吸盘通道二当前的状态信息数据，数据范围 0-255，其中 bit6-7 表示是否检测到物体，状态信息含义与下表相同 |
| Time_Err2 | Int 变量 | 获取的吸盘通道二是否超时报警 |

**说明**：获取钧舵吸盘的状态。

**Contact 状态字含义**（原文，p.328）：

| Bit | 名称 | 值/说明 |
|---|---|---|
| 0 | gAct | 0 表示电动吸盘未使能，1 表示电动吸盘已使能 |
| 2 | gMode | 0 表示处于自动控制模式，1 表示处于高级控制模式 |
| 3 | gGTO | 0 表示停止调节，1 表示正在调节压力或真空度 |
| 4-5 | gSTA | 0 电动吸盘处于未激活状态，1 和 2 未使用，3 表示吸盘已激活 |
| 6-7 | gOBJ | 0 表示气压已低于最低气压；1 表示检测到工件，最低压力值已经达到；2 表示检测到工件，最高压力值已达到；3 表示没有检测到对象，物体已丢失或脱落 |

#### 15.4.19.7 RMRGMGripInit（p.328）

**语法**：`RMRGMGripInit ID`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制电爪运动的电爪 ID |

**说明**：增广电爪 RM-RGM 系列的初始化指令。

#### 15.4.19.8 RMCGripInit（p.328）

**语法**：`RMCGripInit ID`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制电爪运动的电爪 ID |

**说明**：增广电爪 RM-C 系列的初始化指令。

#### 15.4.19.9 RMRGMGripPosMove（p.328）

**语法**：`RMRGMGripPosMove ID,Pos,Vel,Acc,PCheck`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制电爪运动的电爪 ID |
| Pos | Double 变量 | 目标位置，单位 mm，设置范围 -2000.0-2000.0 |
| Vel | Double 变量 | 电爪运行的速度，单位 mm/s，设置范围 0.01-1000.0 |
| Acc | Double 变量 | 电爪运行加速度，单位 mm/s^2，设置范围 0.01-2000.0 |
| PCheck | Double 变量 | 定位范围，单位 mm，设置范围 0.01-10.0 |

**说明**：增广电爪 RMRGM 系列的位置模式的运动指令。

#### 15.4.19.10 RMCGripPosMove（p.329）

**语法**：`RMCGripPosMove ID,Pos,Vel,Acc,PCheck`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制电爪运动的电爪 ID |
| Pos | Double 变量 | 目标位置，单位 mm，设置范围 -2000.0-2000.0 |
| Vel | Double 变量 | 电爪运行的速度，单位 mm/s，设置范围 0.01-1000.0 |
| Acc | Double 变量 | 电爪运行加速度，单位 mm/s2，设置范围 0.01-2000.0 |
| PCheck | Double 变量 | 定位范围，单位 mm，设置范围 0.01-10.0 |

**说明**：增广电爪 RMC 系列的位置模式的运动指令。

#### 15.4.19.11 RMRGMGripTrqMove（p.329）

**语法**：`RMRGMGripTrqMove ID,Pos,Vel,Acc,Trq,PCheck,TCheck`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制电爪运动的电爪 ID |
| Pos | Double 变量 | 目标距离，单位 mm，设置范围 -2000.0-2000.0 |
| Vel | Double 变量 | 电爪运行的速度，单位 mm/s，设置范围 0.01-1000.0 |
| Acc | Double 变量 | 电爪运行加速度，单位 mm/s2，设置范围 0.01-2000.0 |
| Trq | Double 变量 | 定位范围，单位 Nm，设置范围 0.01-100.0 |
| PCheck | Double 变量 | 定位范围，单位 mm，设置范围 0.01-10.0 |
| TCheck | Double 变量 | 时间范围，单位 mm，设置范围 0.01-1000.0（原文如此，TCheck 写“时间范围/单位mm”） |

**说明**：增广电爪 RMRGM 系列的力矩模式的运动指令。

#### 15.4.19.12 RMCGripTrqMove（p.329）

**语法**：`RMCGripTrqMove ID,Pos,Vel,Acc,Trq,PCheck,TCheck`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制电爪运动的电爪 ID |
| Pos | Double 变量 | 目标距离，单位 mm，设置范围 -2000.0-2000.0 |
| Vel | Double 变量 | 电爪运行的速度，单位 mm/s，设置范围 0.01-1000.0 |
| Acc | Double 变量 | 电爪运行加速度，单位 mm/s2，设置范围 0.01-2000.0 |
| Trq | Double 变量 | 定位范围，单位 Nm，设置范围 0.01-100.0 |
| PCheck | Double 变量 | 定位范围，单位 mm，设置范围 0.01-10.0 |
| TCheck | Double 变量 | 时间范围，单位 mm，设置范围 0.01-1000.0（原文如此，与上条同） |

**说明**：增广电爪 RMC 系列的力矩模式的运动指令。

#### 15.4.19.13 RMRGMGripStatus（p.329）

**语法**：`RMRGMGripStatus ID,Pos,Vel,Trq,Reach,Err`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制电爪运动的电爪 ID |
| Pos | Double 变量 | 电爪位置，单位 mm |
| Vel | Double 变量 | 电爪运行的速度，单位 mm/s |
| Trq | Double 变量 | 电爪出力力矩，单位 % |
| Reach | Bool 变量 | 电爪是否到位 |
| Err | Int 变量 | 电爪的错误代码 |

**说明**：增广电爪 RMRGM 系列的获取状态指令。

#### 15.4.19.14 RMCGripStatus（p.330）

**语法**：`RMCGripStatus ID,Pos,Vel,Trq,Reach,Err`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 控制电爪运动的电爪 ID |
| Pos | Double 变量 | 电爪位置，单位 mm |
| Vel | Double 变量 | 电爪运行的速度，单位 mm/s |
| Trq | Double 变量 | 电爪出力力矩，单位 % |
| Reach | Bool 变量 | 电爪是否到位 |
| Err | Int 变量 | 电爪的错误代码 |

**说明**：增广电爪 RMC 系列的获取状态指令。

#### 15.4.19.15 RMRGMResetErr（p.330）

**语法**：`RMRGMResetErr ID`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 需要复位电爪的 ID |

**说明**：增广电爪 RMRGM 系列的复位电爪错误指令。

#### 15.4.19.16 RMCResetErr（p.330）

**语法**：`RMCResetErr ID`

| 项 | 类型 | 说明 |
|---|---|---|
| ID | Int 变量 | 需要复位电爪的 ID |

**说明**：增广电爪 RMC 系列的复位电爪错误指令。

---

## 整理说明（疑点/提取不清清单）

1. `SendByte` 例 4（p.281）：数组声明 `[2]` 却初始化 3 个元素；按 1 起索引 `data2[2]` 取值 17（原文如此）。
2. `ReadBit` 例（p.281）：0x3935 对应位串原文写作 `1001 1100 1010 1100`。
3. `ReadByte`（p.281）：ByteNum 说明写“需要读取的 bit 数量”（原文如此）。
4. `WaitUntil`（p.288）：语法写作 `WaitUntil(cond,\MaxTime,\TimeFlag)`，参数前带反斜杠（原文如此）。
5. `For` 例 1（p.290）：代码为 `from 1 to 10`，说明写“把 i 从 1 到 9 每次加 1 依次打印 9 次”（原文如此）。
6. `Cosh`（p.297）：数学定义式写作 `(exp(x)+exp(x))/2`（原文如此）。
7. 位操作（p.300~302）：`BitRSh` 定义行写作 `BitLSh`；`BitXOr` 示例代码行写作 `BitOr`；`BitSet` 说明句写作“赋值 255”而示例为 `= 0`；`BitLSh`/`BitRSh` 示例拼写为 `BiLSh`/`BiRSh`（均原文如此）。
8. 字符串操作（p.302、305）：`StrFind` 语法参数间无逗号；`StrToInt` 定义行写作 `StrToDouble`（原文如此）。
9. `GetSocketServer` 例（p.283）：注释写“使用 SocketServer 的 attr 属性”，代码判断的是 `ret.state`（原文如此）。
10. `GetRobotState` 例（p.314）：`byte st[4] = GetRobotMaxLoad()`，示例行调用的是 GetRobotMaxLoad（原文如此）。
11. `DynamicThresholdPause`（p.317~318）：定义写 `on/false`，示例写 `DynamicThresholdPause true/false`，注意段写 `true/fasle`（均原文如此）。
12. `RMRGMGripTrqMove`/`RMCGripTrqMove`（p.329）：TCheck 描述为“时间范围，单位 mm”（原文如此）。
13. try/catch 错误码表（p.293~294）：“解释器内部错误”一行仅给出 `0`，无说明与原因文字；区间 `(0. 86400]` 用句点（原文如此）。
14. 15.4.19 末端工具 16 条指令的“示例”栏原文均为空（v2.2 手册）。
15. 第 15.4.7.5 小节分两处呈现：说明与例 1/例 2 在前文，例 3/例 4 在“续”小节。

