# Telegram Chat Harvester

This is an early version of an utility application that allowes you to build a text dataset out of all your Telegram chats. To achieve it, the application exploits [Telegram API](https://github.com/tdlib/td). 

The application is developed mainly to create text corpora. This is not a tool for exporting your Telegram information. See [Why not just export?](#Why_not_just_export?).

## Data Format

This version is capable of collection messages into large text corpora. A single corpus contains all messages in the corresponding chat. The messages are written in the following format:

```
<|cs|>
<|m|>Hi, Mike! How are you?<|--m|>
<|cs|>
<|m|><|--me|>Hi!<|--m|>
<|m|><|--me|>Pretty cool :)<|--m|>
<|m|><|media|><|--m|>
```

Each message is wrapped in "message tokens": `<|m|>` denotes the beginning of a message and `<|--m|>` denotes the end of a message. A start token immediately followed by "author token": `<|--me|>` denotes a start of a message written by you. "Change sender" token `<|cs|>` denotes the end of a block of message written by one user, which is particulary used in groups. Finally, `<|media|>` token is used to denote a message that contains something different from text (sticker, image and etc.).

## Why not just export?

Telegram Desktop can export all the data in machine-readable format (JSON). However, this application provides you with the following functionality that does not 
come with the desktop application:

1. It can be run on systems with no GUI or capability to install the desktop application.
2. It does the necessary preprocessing for producing text corpora.

But, the application cannot export media objects and other data. If you want to export your data, use [Export Tool](https://telegram.org/blog/export-and-more)

## Building

To build an application and run on your desktop machine you can either build application locally (using CMake or any other tool), or build a Docker image.

### Local

To build locally, you have to link [TDLib](https://github.com/tdlib/td) to the executable. Here is the example, how it can be done using CMake

```bash

git clone https://github.com/tdlib/td lib/td
mkdir build
cd build
cmake ..
make
```

It will take awhile to build the library together with the application. After it is done, you can run

```bash

./tx_harvestchats --help

```

### Docker

To build an application with Docker run:

```bash

docker build -t tx-hc .
docker run -it -v /save/results/here:/txhc tx-hc bash -c "cd /txhc && /app/build/tx_harvestchats"

```

The results will be saved in `/save/results/here` folder. Feel free to substitute by your choice.
