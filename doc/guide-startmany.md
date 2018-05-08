#start-many Setup Guide

## Two Options for Setting up your Wallet
There are many ways to setup a wallet to support start-many. This guide will walk through two of them.

1. [Importing an existing wallet (recommended if you are consolidating wallets).](#option1)
2. [Sending 1000 SXN to new wallet addresses.](#option2)

## <a name="option1"></a>Option 1. Importing an existing wallet

This is the way to go if you are consolidating multiple wallets into one that supports start-many. 

### From your single-instance Safenode Wallet

Open your QT Wallet and go to console (from the menu select `Tools` => `Debug Console`)

Dump the private key from your SafeNode's pulic key.

```
walletpassphrase [your_wallet_passphrase] 600
dumpprivkey [mn_public_key]
```

Copy the resulting priviate key. You'll use it in the next step.

### From your multi-instance Safenode Wallet

Open your QT Wallet and go to console (from the menu select `Tools` => `Debug Console`)

Import the private key from the step above.

```
walletpassphrase [your_wallet_passphrase] 600
importprivkey [single_instance_private_key]
```

The wallet will re-scan and you will see your available balance increase by the amount that was in the imported wallet.

[Skip Option 2. and go to Create safenode.conf file](#safenodeconf)

## <a name="option2"></a>Option 2. Starting with a new wallet

[If you used Option 1 above, then you can skip down to Create safenode.conf file.](#safenodeconf)

### Create New Wallet Addresses

1. Open the QT Wallet.
2. Click the Receive tab.
3. Fill in the form to request a payment.
    * Label: mn01
    * Amount: 1000 (optional)
    * Click *Request payment* button
5. Click the *Copy Address* button

Create a new wallet address for each Safenode.

Close your QT Wallet.

### Send 1000 SXN to New Addresses

Just like setting up a standard MN. Send exactly 1000 SXN to each new address created above.

### Create New Safenode Private Keys

Open your QT Wallet and go to console (from the menu select `Tools` => `Debug Console`)

Issue the following:

```safenode genkey```

*Note: A safenode private key will need to be created for each Safenode you run. You should not use the same safenode private key for multiple Safenodes.*

Close your QT Wallet.

## <a name="safenodeconf"></a>Create safenode.conf file

Remember... this is local. Make sure your QT is not running.

Create the `safenode.conf` file in the same directory as your `wallet.dat`.

Copy the safenode private key and correspondig collateral output transaction that holds the 1000 SXN.

The safenode private key may be an existing key from [Option 1](#option1), or a newly generated key from [Option 2](#option2). 

*Note: The safenode priviate key is **not** the same as a wallet private key. **Never** put your wallet private key in the safenode.conf file. That is almost equivalent to putting your 1000 SXN on the remote server and defeats the purpose of a hot/cold setup.*

### Get the collateral output

Open your QT Wallet and go to console (from the menu select `Tools` => `Debug Console`)

Issue the following:

```safenode outputs```

Make note of the hash (which is your collateral_output) and index.

### Enter your Safenode details into your safenode.conf file
[From the safenode github repo](https://github.com/safenodecoin/safenodecoin/blob/master/doc/safenode_conf.md)

`safenode.conf` format is a space seperated text file. Each line consisting of an alias, IP address followed by port, safenode private key, collateral output transaction id and collateral output index.

```
alias ipaddress:port safenode_private_key collateral_output collateral_output_index
```

Example:

```
mn01 127.0.0.1:9999 93HaYBVUCYjEMeeH1Y4sBGLALQZE1Yc1K64xiqgX37tGBDQL8Xg 2bcd3c84c84f87eaa86e4e56834c92927a07f9e18718810b92e0d0324456a67c 0
mn02 127.0.0.2:9999 93WaAb3htPJEV8E9aQcN23Jt97bPex7YvWfgMDTUdWJvzmrMqey aa9f1034d973377a5e733272c3d0eced1de22555ad45d6b24abadff8087948d4 0
```

## What about the safenode.conf file?

If you are using a `safenode.conf` file you no longer need the `safenode.conf` file. The exception is if you need custom settings (_thanks oblox_). In that case you **must** remove `safenode=1` from local `safenode.conf` file. This option should be used only to start local Hot safenode now.

## Update safenode.conf on server

If you generated a new safenode private key, you will need to update the remote `safenode.conf` files.

Shut down the daemon and then edit the file.

```nano .safenode/safenode.conf```

### Edit the safenodeprivkey
If you generated a new safenode private key, you will need to update the `safenodeprivkey` value in your remote `safenode.conf` file.

## Start your Safenodes

### Remote

If your remote server is not running, start your remote daemon as you normally would. 

You can confirm that remote server is on the correct block by issuing

```safenode-cli getinfo```

and comparing with the official explorer at https://explorer.safenodecoin.info/chain/SafeNode

### Local

Finally... time to start from local.

#### Open up your QT Wallet

From the menu select `Tools` => `Debug Console`

If you want to review your `safenode.conf` setting before starting Safenodes, issue the following in the Debug Console:

```safenode list-conf```

Give it the eye-ball test. If satisfied, you can start your Safenodes one of two ways.

1. `safenode start-alias [alias_from_safenode.conf]`  
Example ```safenode start-alias mn01```
2. `safenode start-many`

## Verify that Safenodes actually started

### Remote

Issue command `safenode status`
It should return you something like that:
```
safenode-cli safenode status
{
    "vin" : "CTxIn(COutPoint(<collateral_output>, <collateral_output_index>), scriptSig=)",
    "service" : "<ipaddress>:<port>",
    "pubkey" : "<1000 SXN address>",
    "status" : "Safenode successfully started"
}
```
Command output should have "_Safenode successfully started_" in its `status` field now. If it says "_not capable_" instead, you should check your config again.

### Local

Search your Safenodes on https://safenodeninja.pl/safenodes.html

_Hint: Bookmark it, you definitely will be using this site a lot._