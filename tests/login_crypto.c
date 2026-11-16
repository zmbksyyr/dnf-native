#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "crypto.h"
#include "builtin_key.h"

static const char *UIDS[] = {"10001","10002","12345","99999999"};
static const char *EXPECT_B64[] = {
"DwMYtxG78APNdRrIzE3OFU8lsrUV+kVKZ91L2WZt1DdE9i6+S5U6+Xr4vWswqWn85Gfl97QPWVK4e9KhEmKMzIVW4ij7dpOuwh02NIBe2xh792wfhikcXw1Z/kEadVywsAATVFLYNEyKDkGSeD4PWmI9vRrAxsqzMOvnLjdcLGDr2DbvHAtlnbVCPdCozITB8rW2IAOcZw87mA9ZNK05fue3cXDSNxOcdqPHeSRNdWvrKD0WxTQECjiRspd8PcvZ8FcVCBnptY/mQFkjffwd/MWoQDhQeo6CTmhvRWG/bBAnAquV49SgqagFf/KFywXO2FkOE7NpEn6jBObhd73vMg==",
"lPwYThcN8NmGgFQUU/JrUzlVZhOfy4+M6sqA9g7jP0tpWyC4QISTnIc7sjT4Sf+Nw0BJSfIycmxqyfkBLoO/NZEWXWfP6sH8/i6nrDsuTAfQlpIiHW3dvozA9UuyhjgHlXzMT5ECicg2/CVcVM48h8UEwpDYDQsCP2dzIwExcaIyQbB4L7y7jPXodmbwfZ++aw9sDvfBmfZnV479zz52HhwSZyPMuMnJAb1wIgyUBOLoz/arc+bdPbxUy4CGmeTw8uxIgFGTwRn+3ij4emeG321wJ6N8+fSOTX/uGSapUoC9mfheIDohUrcK4Ftf6TRwh7cTJ5L10LoNV24yidftfA==",
"hqmdhaRiO14WArvahdDWfSbOO8sRDkZY2mjIzWjBzvezDBt8WEItdxkvrqJefLqeaJ51rbS5w1huc42fZIXU1Fv49YB34UYfiNMTBDwdps8ErmyEVJBzR4vb9Lc+vTpY/iox2RDQgwLKlSxpHeyG9vw35+jezudXIRpv8+LwcCRCTIYnlqnmlvSQEYRNoRg+WvASaqvkC/ainxfuFAs5e8i/B4AwvyhFLqFHPdZT40g5VPUHWi90TGEZVQO5rR2M02ZNAU9sspsqQOWZeOOOB5eg0C6efyPhM6eMz0/VG7ZOso3YbiZaEbHgjnkmGAo27q4l8WNTCz+OOlZJWx5sLA==",
"NPuFBG7c0zGLwGSpW6pzbAMWpLXInOSU2YGimOnLvYMiigVRUY7HLOxr1NFLVt8TU/fTotPSfR8O74mN0X44pRIepkjQAxwKz+cfx8QzkFFwQprUQhx80qHiOKtXOByI8yvuYXBypzTN/dbGPa1COeWUAn8wAr49zBkSVp5vdFB7Tlo0JT208Lj9l+ERdzB0zvp/CftoBk/XuGlU2Eg5Oy1c83ThyUoC8Qsv09kVE8ducoO2i71Zp9GMIbUdxmq0mRYQgHdxY6BxO4sotycIFrw4bJxIKsqy0xZ1IYnPzd/nnVimoPuOy3b/RmUaKaiEvM9b3gpojV7tw9mEXomtXQ=="
};

int main(int argc, char **argv){
    char mh[33]; md5_hex((const uint8_t*)"123456",6,mh);
    printf("MD5=%s %s\n", mh, strcmp(mh,"e10adc3949ba59abbe56e057f20f883e")==0?"OK":"FAIL");

    RsaKey key;
    if (rsa_key_from_pem(BUILTIN_PRIVATE_KEY, sizeof(BUILTIN_PRIVATE_KEY)-1, &key)!=0){
        printf("key parse FAIL\n"); return 1;
    }
    printf("key k=%d limbs=%d\n", key.k, key.limbs);

    const char *uidt="010101010101010101010101010101010101010101010101010101010101010155914510010403030101";
    int pass=strcmp(mh,"e10adc3949ba59abbe56e057f20f883e")==0;
    for (int idx=0; idx<4; idx++){
        unsigned long long ui = strtoull(UIDS[idx],NULL,10);
        char us[32]; sprintf(us,"%llx",ui);
        int l=(int)strlen(us);
        char tt[200]; int t=0;
        for (int i=0;i<8-l;i++) tt[t++]='0';
        memcpy(tt+t,us,l); t+=l;
        strcpy(tt+t,uidt); t+=(int)strlen(uidt);

        uint8_t dst[128]; int dlen=0;
        for (int i=0;i<t;i+=2){
            unsigned int x; sscanf(tt+i,"%2x",&x); dst[dlen++]=(uint8_t)x;
        }
        uint8_t enc[512];
        int eLen = rsa_private_encrypt(&key,dst,(size_t)dlen,enc);
        if (eLen < 0) return 1;
        char b64[1024]; base64_encode(enc,(size_t)eLen,b64);
        int ok = strcmp(b64,EXPECT_B64[idx])==0;
        printf("UID=%s encLen=%d %s\n", UIDS[idx], eLen, ok?"OK":"FAIL");
        if(!ok){ printf(" got: %s\n want: %s\n", b64, EXPECT_B64[idx]); pass=0; }
    }
    if (argc == 3 && strcmp(argv[1], "--export-public") == 0) {
        char public_pem[1024];
        if (rsa_public_key_pem(BUILTIN_PRIVATE_KEY, sizeof(BUILTIN_PRIVATE_KEY)-1,
                               public_pem, sizeof(public_pem))) return 1;
        FILE *file = fopen(argv[2], "wb");
        if (!file) return 1;
        size_t size = strlen(public_pem);
        int ok = fwrite(public_pem, 1, size, file) == size;
        if (fclose(file)) ok = 0;
        if (!ok) return 1;
    }
    printf(pass?"ALL OK\n":"SOME FAIL\n");
    return pass?0:1;
}
