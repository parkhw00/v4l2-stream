# V4L2-Stream

Stream V4L2 camera to Web Browser

## Prerequisites

* gstreamer1.0
* libwebsocketpp
* libboost-system
* pkg-config
* build-essential
* python3 (optional, for a simple web server hosting static files)

Install with

```
$ sudo apt install libgstreamer1.0-dev libwebsocketpp-dev pkg-config build-essential libboost-system-dev python3
```

## Download & Build

```
$ git clone https://github.com/mbyzhang/v4l2-stream.git
$ cd v4l2-stream
$ make
```

## Usage

Start stream server with

```
$ ./v4l2-streamer -d /dev/video0 -w 1280 -h 720 -f 30
```

and start Web server with

```
$ cd v4l2-stream/public
$ python3 -m http.server
```

and open [http://localhost:8000/](http://localhost:8000/) to see the stream.

## Third-party notices

```
Broadway.js - https://github.com/mbebenita/Broadway

Copyright (c) 2011, Project Authors (see AUTHORS file)
All rights reserved.

```
