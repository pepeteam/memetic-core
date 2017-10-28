# PepeCoin - PEPE 

v. 3.0.0.0 Rebase
####
Pepecoin is digital decentralized crypto-currency, a hybrid zombie mix of seven other cryptocurrency projects, plus a lot of custom code and goals.

Pepecoin incorporates both blockchain 2.0 and blockchain 3.0 technologies and is under continued development.

Pepecoin is a Proof of Work (POW) /  Proof of Stake (POS) cryptocurrency fork of 6 different cryptocurrencies with many security additions.

PepeCoin is decentralized, encrypted image storage in IPFS, cryptographically verified against the PepeCoin blockchain. Protect your memes and political information from censorship.

PepeCoin is blockchain hash secured image / archive storage, with decentralized moderation ability.

PepeCoin is a decentralized, encrypted project that supports the preservation of all freedom of speech within the bounds of US law.


***

![PepeCoin Logo](http://i.imgur.com/hDXKtDB.png  "PepeCoin Logo")

***
**Blockchain Specifications**

 - Default port 29377 
 - RPC port 29376
 - 60 seconds block time
 - X11 mining algorithm
 - Proof of Stake - POS - 7% at block 600,000, drops by 2% yearly and stays at 3%
 - Proof of Work - POW - Restarts at block 600,000, X11  See [ANN](https://bitcointalk.org/index.php?topic=1391598.0) for more details.
  - 40 confirmations for newly Proof of Work mined blocks
 - 100 confirmations for newly Proof of Stake minted blocks
 - 60 seconds per block time Proof of Stake
  - 6 hour minimum stake age (12 hour average)
  - Additional security improvements
  
 **Features**

- Decentralized encrypted private messaging
- Stealth Addresses
- Fully blockchain based, public decentralized encrypted messaging chat wall in wallet
- Integrated Bittrex trading functions
- Integrated block explorer
- Image timestamping on the blockchain for proof of ownership
- In-wallet hash image verification checking functions
- JSON-RPC functions to externally query blockchain for hashed image datas
- In wallet public messaging wall
- JSON-RPC functions to externally query all public wall messages
- IPFS-based  data hashing storage currently under development
- Masternode functionality in progress, expected node ownership fee of 50k PEPE
See BitcoinTalk post for more info: https://bitcointalk.org/index.php?topic=1391598.0


-------------------------------
Additional Information
------------------------------
 
**Windows Wallet**

https://github.com/pepeteam/pepecoin/releases


**Linux Builds:**

PepeCoin uses libsecp256k1, libgmp, Boost1.55+, Openssl1.01p, Berkeley DB 4.8+, QT5 to compile


	sudo apt-get install build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils libboost-system-dev libboost-filesystem-dev libboost-chrono-dev libboost-program-options-dev libboost-test-dev libboost-thread-dev libminiupnpc-dev libgmp3-dev libdb-dev libdb++-dev libgmp3-dev
	git clone https://github.com/pepeteam/pepecoin.git
	cd pepecoin/src
	make -f makefile.unix USE_UPNP=-1

To build the GUI with Qt 5 you need the following:

    sudo apt-get install libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler qt5 qt5-default
    git clone https://github.com/pepeteam/pepecoin.git
    cd pepecoin 
    qmake


***
![](http://i.imgur.com/EfYNxub.png) 
***

--------------------
Links
--------------------

**Information**
[Bitcointalk ANN](https://bitcointalk.org/index.php?topic=1391598.0) 

Windows Wallet:  [Download](https://github.com/pepeteam/pepecoin/releases) 


**Website**
https://memetic.ai
https://pepecoin.co


**Exchanges**

[https://bittrex.com/Market/Index?MarketName=BTC-MEME  ](https://bittrex.com/Market/Index?MarketName=BTC-MEME  ) 
[https://www.cryptopia.co.nz/Exchange?market=PEPE_BTC](https://www.cryptopia.co.nz/Exchange?market=PEPE_BTC)
[https://www.coinexchange.io/market/MEME/BTC](https://www.coinexchange.io/market/MEME/BTC) 


**Block Explorers**

[http://memetic.thecryptochat.net/](http://memetic.thecryptochat.net/) 
[https://prohashing.com/explorer/Memeticcoin/](https://prohashing.com/explorer/Memeticcoin/) 


**Charts**

[https://coinmarketcap.com/currencies/pepecoin/](https://coinmarketcap.com/currencies/pepecoin/) 
https://www.worldcoinindex.com/coin/memetic
https://www.cryptocompare.com/coins/meme/charts/BTC
https://bitinfocharts.com/markets/bittrex/meme-btc-1m.html
https://cryptocoinview.com/MEME


**Social**
[https://twitter.com/pepecoins](https://twitter.com/pepecoins) https://twitter.com/pepecoins
Telegram group:  [Get invite](https://t.me/jpepecoins) 


***

**Nodes**
addnode=seed.kekdaq.com
addnode=seed.pepecoin.co

![pepecoin-256](https://i.imgur.com/xnSJvT9.jpg  "pepecoin-256")

------------------
License
------------------

>Copyright (c) 2009-2010 Satoshi Nakamoto
>Copyright (c) 2009-2012 The Bitcoin developers
>Copyright (c) 2012 Litecoin Developers
>Copyright (c) 2013 Peercoin Developers
>Copyright (c) 2014 DarkCoin Developers
>Copyright (c) 2014 BlackCoin Developers
>Copyright (c) 2014 Digibyte Developers
>Copyright (c) 2014 DashCoin Developers
>Copyright (c) 2014 NetCoin Developers
>Copyright (c) 2015 PepeCoin Developers
>Copyright (c) 2015-2016 PepeCoin Developers
>Copyright (c) 2015-2016 Memetic Developers
>Copyright (c) 2017 PepeCoin Developers
>
>Permission is hereby granted, free of charge, to any person obtaining a copy
>of this software and associated documentation files (the "Software"), to deal
>in the Software without restriction, including without limitation the rights
>to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
>copies of the Software, and to permit persons to whom the Software is
>furnished to do so, subject to the following conditions:
>
>The above copyright notice and this permission notice shall be included in
>all copies or substantial portions of the Software.
>
>THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
>IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
>FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
>AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
>LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
>OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
>THE SOFTWARE.
>
