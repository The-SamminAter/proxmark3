//-----------------------------------------------------------------------------
// Copyright (C) 2021 Merlok
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// CIPURSE crypto primitives
//-----------------------------------------------------------------------------

#include "cipursecrypto.h"

#include "commonutil.h"  // ARRAYLEN
#include "comms.h"       // DropField
#include "util_posix.h"  // msleep

#include "cmdhf14a.h"
#include "emv/emvcore.h"
#include "emv/emvjson.h"
#include "crypto/libpcrypto.h"
#include "ui.h"
#include "util.h"

uint8_t AESData0[CIPURSE_AES_KEY_LENGTH] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void bin_xor(uint8_t *d1, uint8_t *d2, size_t len) {
    for(size_t i = 0; i < len; i++)
        d1[i] = d1[i] ^ d2[i];
}

static void bin_ext(uint8_t *dst, size_t dstlen, uint8_t *src, size_t srclen) {
    if (srclen > dstlen)
        memcpy(dst, &src[srclen - dstlen], dstlen);
    else
        memcpy(dst, src, dstlen);
}

static void bin_pad(uint8_t *dst, size_t dstlen, uint8_t *src, size_t srclen) {
    memset(dst, 0, dstlen);
    if (srclen <= dstlen)
        memcpy(&dst[dstlen - srclen], src, srclen);
    else
        memcpy(dst, src, dstlen);
}

static void bin_pad2(uint8_t *dst, size_t dstlen, uint8_t *src, size_t srclen) {
    memset(dst, 0, dstlen);
    uint8_t dbl[srclen * 2];
    memcpy(dbl, src, srclen);
    memcpy(&dbl[srclen], src, srclen);
    bin_pad(dst, dstlen, dbl, srclen * 2);
}

static uint64_t rotateLeft48(uint64_t src) {
    uint64_t dst = src << 1;
    if (dst & 0x0001000000000000UL) {
        dst = dst | 1;
        dst = dst & 0x0000ffffffffffffUL;
    }
    return dst;
}

static uint64_t computeNLM48(uint64_t x, uint64_t y) {
    uint64_t res = 0;
    
    for (int i = 0; i < 48; i++) {
        res = rotateLeft48(res);
        if (res & 1)
            res = res ^ CIPURSE_POLY;
        y = rotateLeft48(y);
        if (y & 1)
            res = res ^ x;    
    }
    return res;
}

static void computeNLM(uint8_t *res, uint8_t *x, uint8_t *y) {
    uint64_t x64 = 0;
    uint64_t y64 = 0;
    
    for (int i = 0; i < 6; i++) {
        x64 = (x64 << 8) | x[i];
        y64 = (y64 << 8) | y[i];
    }
    
    uint64_t res64 = computeNLM48(x64, y64);
    
    for (int i = 0; i < 6; i++) {
        res[5 - i] = res64 & 0xff;
        res64 = res64 >> 8;
    }
}

static void CipurseCGenerateK0AndCp(CipurseContext *ctx) {        
    uint8_t temp1[CIPURSE_AES_KEY_LENGTH] = {0};
    uint8_t temp2[CIPURSE_AES_KEY_LENGTH] = {0};
    uint8_t kp[CIPURSE_SECURITY_PARAM_N] = {0};
        
    // session key derivation function
    // kP := NLM(EXT(kID), rP)
    // k0 := AES(key=PAD2(kP) XOR PAD(rT),kID) XOR kID
    bin_ext(temp1, CIPURSE_SECURITY_PARAM_N, ctx->key, CIPURSE_AES_KEY_LENGTH);
    computeNLM(kp, ctx->rP, temp1);  // param sizes == 6 bytes
    bin_pad2(temp1, CIPURSE_AES_KEY_LENGTH, kp, CIPURSE_SECURITY_PARAM_N);
    bin_pad(temp2, CIPURSE_AES_KEY_LENGTH, ctx->rT, CIPURSE_SECURITY_PARAM_N);
    bin_xor(temp1, temp2, CIPURSE_AES_KEY_LENGTH);
    
    // session key K0    
    aes_encode(NULL, temp1, ctx->key, ctx->k0, CIPURSE_AES_KEY_LENGTH);
    bin_xor(ctx->k0, ctx->key, CIPURSE_AES_KEY_LENGTH);
        
    // first frame key k1, function to calculate k1,
    // k1 := AES(key = RP; k0 XOR RT) XOR (k0 XOR RT)
    memcpy(temp1, ctx->k0, CIPURSE_AES_KEY_LENGTH);
    bin_xor(temp1, ctx->RT, CIPURSE_AES_KEY_LENGTH);
    aes_encode(NULL, ctx->RP, temp1, temp2, CIPURSE_AES_KEY_LENGTH);
    bin_xor(temp1, temp2, CIPURSE_AES_KEY_LENGTH);
    memcpy(ctx->frameKey, temp1, CIPURSE_AES_KEY_LENGTH);
        
    // function to caluclate cP := AES(key=k0, RP).
    // terminal response
    aes_encode(NULL, ctx->k0, ctx->RP, ctx->cP, CIPURSE_AES_KEY_LENGTH);
}

static void CipurseCGenerateCT(uint8_t *k0, uint8_t *RT, uint8_t *CT) {
    aes_encode(NULL, k0, RT, CT, CIPURSE_AES_KEY_LENGTH);
}

// from: https://github.com/duychuongvn/cipurse-card-core/blob/master/src/main/java/com/github/duychuongvn/cirpusecard/core/security/securemessaging/CipurseSecureMessage.java#L68
void CipurseCGetKVV(uint8_t *key, uint8_t *kvv) {
    uint8_t res[16] = {0};
    aes_encode(NULL, key, AESData0, res, CIPURSE_AES_KEY_LENGTH);
    memcpy(kvv, res, CIPURSE_KVV_LENGTH);
}

void CipurseCClearContext(CipurseContext *ctx) {
    if (ctx == NULL)
        return;
    
    memset(ctx, 0, sizeof(CipurseContext));
}

void CipurseCSetKey(CipurseContext *ctx, uint8_t keyId, uint8_t *key) {
    if (ctx == NULL)
        return;
    
    CipurseCClearContext(ctx);
    
    ctx->keyId = keyId;
    memcpy(ctx->key, key, member_size(CipurseContext, key));
}

void CipurseCSetRandomFromPICC(CipurseContext *ctx, uint8_t *random) {
    if (ctx == NULL)
        return;
      
    memcpy(ctx->RP, random, member_size(CipurseContext, RP));
    memcpy(ctx->rP, random + member_size(CipurseContext, RP), member_size(CipurseContext, rP));
}

void CipurseCSetRandomHost(CipurseContext *ctx) {
    memset(ctx->RT, 0x10, member_size(CipurseContext, RT));
    memset(ctx->rT, 0x20, member_size(CipurseContext, rT));
}

static void CipurseCFillAuthData(CipurseContext *ctx, uint8_t *authdata) {
    memcpy(authdata, ctx->cP, member_size(CipurseContext, cP));
    memcpy(&authdata[member_size(CipurseContext, cP)], ctx->RT, member_size(CipurseContext, RT));
    memcpy(&authdata[member_size(CipurseContext, cP) + member_size(CipurseContext, RT)], ctx->rT, member_size(CipurseContext, rT));   
}

void CipurseCAuthenticateHost(CipurseContext *ctx, uint8_t *authdata) {
    if (ctx == NULL)
        return;
    
    CipurseCSetRandomHost(ctx);
    CipurseCGenerateK0AndCp(ctx);
    CipurseCGenerateCT(ctx->k0, ctx->RT, ctx->CT);

    if (authdata != NULL)
        CipurseCFillAuthData(ctx, authdata);
}

bool CipurseCCheckCT(CipurseContext *ctx, uint8_t *CT) {
    return (memcmp(CT, ctx->CT, CIPURSE_AES_KEY_LENGTH) == 0);
}
