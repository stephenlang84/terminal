////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2017, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#ifndef _BIP32_SERIALIZATION_H
#define _BIP32_SERIALIZATION_H

#include "BtcUtils.h"
#include "EncryptionUtils.h"
#include "btc/bip32.h"

class BIP32_Node
{
private:
   SecureBinaryData chaincode_;
   SecureBinaryData privkey_;
   SecureBinaryData pubkey_;

   btc_hdnode node_;

private:
   SecureBinaryData encodeBase58(void) const;
   void decodeBase58(const char*);
   void init(void);
   void assign(void);

public:
   BIP32_Node(void)
   {}

   void initFromSeed(const SecureBinaryData&);
   void initFromBase58(const SecureBinaryData&);
   void initFromPrivateKey(uint8_t depth, unsigned leaf_id,
      const SecureBinaryData& privKey, const SecureBinaryData& chaincode);
   void initFromPublicKey(uint8_t depth, unsigned leaf_id,
      const SecureBinaryData& pubKey, const SecureBinaryData& chaincode);

   //gets
   SecureBinaryData getBase58(void) const { return encodeBase58(); }
   uint8_t getDepth(void) const { return node_.depth; }
   uint32_t getFingerPrint(void) const { return node_.fingerprint; }
   unsigned getLeafID(void) const { return node_.child_num; }
   BIP32_Node getPublicCopy(void) const;

   const SecureBinaryData& getChaincode(void) const { return chaincode_; }
   const SecureBinaryData& getPrivateKey(void) const { return privkey_; }
   const SecureBinaryData& getPublicKey(void) const { return pubkey_; }

   SecureBinaryData&& moveChaincode(void) { return std::move(chaincode_); }
   SecureBinaryData&& movePrivateKey(void) { return std::move(privkey_); }
   SecureBinaryData&& movePublicKey(void) { return std::move(pubkey_); }

   //sets
   /*void setPublicKey(const SecureBinaryData&);
   void setPrivateKey(const SecureBinaryData&);
   void setChaincode(const SecureBinaryData&);*/

   //derivation
   void derivePrivate(unsigned);
   void derivePublic(unsigned);

   bool isPublic(void) const;
};

#endif