##idaq
Developping...

##Usage

###接收端
例：

接收端（192.168.1.7）：

```
idaq -s 1 -p 7777
```

参数：

- -s：接收端类型。1 是只能接收和处理单个发送端发来的连接；2 是可以接收多个发送端发来的连接，但是采用先来先服务（FCFS）的方式处理这些连接，处理完一个再处理下一个；3 是可以接收多个发送端发来的连接，采用多线程并发处理这些连接，为每个连接建立一个线程来处理。
- -p：设定接收端接收连接的端口号。发送端必须设定一致的端口号才能建立起连接。

##发送端
例：
一级发送端（192.168.1.5）和二级发送端（192.168.1.6）：

```
idaq -c 1 -a 192.168.1.6 -p 6666 -size 128 -t 10
idaq -c 4 -a 192.168.1.7 -p 7777 -P 6666
```

参数：

- -c：发送端的类型。1 是普通的发送端，只发送数据；2 或 3 或 4 是中间发送端，接收来自上一级发送端的数据，并转发给下一级。2 是只能接收和处理单个发送端发来的连接，接收并转发数据到下一级；3 是可以接收多个发送端发来的连接，接收并转发数据到下一级，但是采用先来先服务（FCFS）的方式处理这些连接，处理完一个再处理下一个；4 是可以接收多个发送端发来的连接，接收并转发数据到下一级，采用多线程并发处理这些连接，为每个连接建立一个线程来处理。
- -a：目标接收端的 IP 地址。
- -p：目标接收端的端口号。
- -size：当发送端类型为 1 时，设置这个参数。发送数据包的大小，单位为字节。
- -t：当发送端类型为 1 时，设置这个参数。发送数据包总的时间。
- -P：当发送端类型为 2 时，设置这个参数。接收上一级发送端连接的端口号。

##示例
###1、两级测试
单个发送端单线程发送数据到接收端，接收端单线程接收数据。

发送端（192.168.1.5）：

```
idaq -c 1 -a 192.168.1.6 -p 6666 -size 128 -t 10
```

接收端（192.168.1.6）：

```
idaq -s 1 -p 6666
```

###2、三级测试
多个一级发送端同时发送数据到一个二级发送端，二级发送端多线程接收并转发数据到接收端，接收端多线程接收数据。

一级发送端，3 个同时发送（192.168.1.5，192.168.1.6，192.168.1.7）：

```
idaq -c 1 -a 192.168.1.8 -p 8888 -size 128 -t 10
```

二级发送端（192.168.1.8）：

```
idaq -c 4 -a 192.168.1.7 -p 9999 -P 8888
```

接收端（192.168.1.9）：

```
idaq -s 3 -p 9999
```


##MIT Licence
Copyright (c) 2014 Samir Chen

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
