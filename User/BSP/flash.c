/**
  ******************************************************************************
  * @file    flash.c
  * @brief   内部 Flash 配置备份模块实现(定时配置的掉电保存 + CRC32 完整性校验)
  ******************************************************************************
  * 存储位置
  *   - 配置存于片内 Flash 的**最后一页**: FLASH_CFG_ADDR = 0x0807F800,
  *     页大小 2KB(STM32F103 大容量产品, 每页 2KB, 共 512KB);
  *   - 该页不属于程序代码区, 擦写它不会破坏固件;
  *   - 只占 400 字节, 远小于 2KB, 因此整页擦除不会影响其它数据。
  *
  * 数据结构(共 400 字节, 布局必须与 flash.h 严格一致)
  *   - magic : 0xA5A5C7C7, "这片 Flash 里存的是本模块数据"的标志;
  *   - rules : 7 个药盒 x 7 天 x 每天最多 4 个时间点;
  *   - crc   : 仅覆盖前 396 字节(magic + rules), 不含 crc 自身。
  *
  * 完整性校验(两级)
  *   1. magic 相等  —— 判断 Flash 里是否是本程序写过的数据;
  *   2. CRC32 相等  —— 判断数据在掉电/擦写中断中是否被破坏。
  *   任意一级不过都返回 0, 由上层 App_Init() 回落默认配置。
  *
  * 为什么必须校验而不能只信任 Flash
  *   - 写入过程可能被复位/掉电打断(擦除整页后只写了一半);
  *   - 新烧录的固件, 该页内容是 0xFF(全擦除态), magic 必然不匹配;
  *   - 没有校验的话, 半截数据会被当成合法配置读出,
  *     结果就是"药盒在莫名其妙的时间响", 比直接回落默认值危险得多。
  ******************************************************************************
  */
#include "flash.h"
#include <stddef.h>
#include <string.h>

/* 全局定时配置 */
TimerCfg_t g_Cfg;

/* ==================== CRC32 查找表 ====================
 *
 * 表的作用: 把"逐位"计算 CRC 的 8 次循环压成"查一次表"。
 *   - 逐位法: 每处理 1 个字节要做 8 轮移位/判断/异或;
 *   - 查表法: 预先把 256 种字节取值的 8 轮结果算好存成这张表,
 *     运行时每个字节只需一次移位 + 一次异或 + 一次查表。
 *
 * 参数约定(与 zlib/gzip/PNG 用的标准 CRC-32/ISO-HDLC 完全相同)
 *   - 生成多项式: 0x04C11DB7; 因采用**反射(小端)算法**, 表里使用的是
 *     它的位反转形式 0xEDB88320;
 *   - 输入反射 = 真, 输出反射 = 真;
 *   - 初值 = 0xFFFFFFFF, 最终结果再异或 0xFFFFFFFF。
 *
 * 常用参考值(可用于验证本表/本函数是否正确)
 *   - "123456789" 的 CRC32 = 0xCBF43926(标准测试向量);
 *   - Flash 为全 0xFF(空白页)时, 对前 396 字节算得的 CRC = 0xC3D0643C
 *     (注意**不是** 0xFFFFFFFF —— 0xFFFFFFFF 是初值, 末步还要异或取反;
 *      这个值可用同一张表在 PC 上重算验证, 见 tools/verify_numbers.py)。
 *   - 另: 全 0x00 与全 0xFF 的 CRC 不同, 因此可区分「页被擦过」与「页被写成全 0」。
 *
 * 表项含义: s_CrcTable[b] = 把字节 b 作为当前 CRC 的低 8 位,
 *           做满 8 次"右移+条件异或"之后的结果。
 *
 * 注意: 这张表的 256 个常量是数据, 不是算法, 一个字节都不能动;
 *       改错任何一个常量, 都会导致 CRC 校验规律性失败。
 */
static const uint32_t s_CrcTable[256] =
{
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du
};

/**
  * @brief  计算标准 CRC-32/ISO-HDLC 校验值
  * @param  buf 待校验数据首地址(可为 Flash 地址, 读操作无需解锁)
  * @param  len 待校验字节数
  * @retval 32 位 CRC 值
  *
  * 原理(查表法, 每次消掉输入的一个字节)
  *   1. crc 初值取 0xFFFFFFFF(与标准一致, 避免前导 0 不影响结果);
  *   2. 每个字节: crc = (crc >> 8) ^ s_CrcTable[(crc ^ buf[i]) & 0xFF];
  *      - (crc ^ buf[i]) & 0xFF 取出"当前 CRC 低 8 位与输入字节"的异或,
  *        它就是这一步该查表的索引, 正好等价于逐位法 8 轮迭代的结果;
  *      - crc >> 8 表示这 8 轮移入的 8 个零比特(反射算法的方向是右移);
  *      - 两者异或即完成 8 位推进。
  *   3. 最后再异或 0xFFFFFFFF 取反输出。
  *
  * 为什么用它而不是简单累加和
  *   - 累加和对"字节顺序颠倒"不敏感, CRC 对位错/字节丢失极灵敏;
  *   - 表只占 1KB 常量空间, 每次校验 396 字节在 72MHz 下只要几十微秒,
  *     上电初始化时算一次完全不构成负担。
  */
uint32_t Flash_CRC32(const uint8_t *buf, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        crc = (crc >> 8) ^ s_CrcTable[(crc ^ buf[i]) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

/**
  * @brief  从 Flash 读取定时配置并校验完整性
  * @param  cfg 输出缓冲, 由调用者提供, 大小必须 >= sizeof(TimerCfg_t)
  * @retval 1 = 数据有效且已填入 cfg
  * @retval 0 = 数据无效(首次上电 / 被擦除 / 已损坏), cfg 内容无意义, 勿使用
  *
  * 原理(两级校验, 顺序不能颠倒)
  *   1. 先把整块 400 字节读出来。读操作不需要 FLASH_Unlock(),
  *      片内 Flash 的读就像普通内存访问一样自由;
  *   2. 比较 magic: 不等说明这页根本没被本程序写过(例如刚烧录完固件,
  *      该页还是全 0xFF), 此时连 CRC 都没必要算, 直接判无效;
  *   3. 再比较 CRC: 只算前 offsetof(crc) = 396 字节, 也就是 magic + rules。
  *      若把 crc 字段本身也算进去, 等式永远不成立。
  *
  * 为什么必须两层而不能只看 magic
  *   - magic 是常量, 掉电写坏的位置如果恰好落在 magic 之外的字节,
  *     magic 仍然相等, 于是"看起来有效"的脏数据会被当真;
  *   - 加上 CRC 后, 只要 396 字节里错 1 个比特就能被发现。
  */
uint8_t Flash_ReadConfig(TimerCfg_t *cfg)
{
    const uint8_t *p = (const uint8_t *)FLASH_CFG_ADDR;

    memcpy(cfg, p, sizeof(TimerCfg_t));  /* 直接按字节整体搬到 RAM, 之后所有判断都在 RAM 上做,
                                            避免中途 Flash 内容改变导致"两次读到的 magic 不一致" */

    if (cfg->magic != FLASH_CFG_MAGIC)
    {
        return 0;
    }
    if (cfg->crc != Flash_CRC32((const uint8_t *)cfg, offsetof(TimerCfg_t, crc)))
    {
        return 0;
    }
    return 1;
}

/**
  * @brief  把定时配置写入 Flash(自动重算 CRC + 回读校验)
  * @param  cfg 待保存的配置; 其 crc 字段会被忽略并覆盖
  * @retval 1 = 保存成功且回读一致
  * @retval 0 = 失败(擦除失败 / 编程失败 / 回读不一致), 旧配置已被破坏
  *
  * 写入流程(顺序不可调换)
  *   1. 先整块拷到栈上局部变量 tmp —— 只读参数指针, 不改调用者的数据;
  *   2. 用 tmp 前 396 字节重算 CRC 并回填 tmp.crc,
  *      所以调用者不必(也不应)自己维护 crc;
  *   3. FLASH_Unlock() —— 解除 Flash 控制寄存器的写保护;
  *   4. FLASH_ErasePage() 先擦整页 —— Flash 只能把 1 写成 0,
  *      要想把 0 写回 1 必须擦除, 而 F1 的最小擦除单位是整页 2KB;
  *   5. 半字循环写 200 次, 覆盖全部 400 字节;
  *   6. FLASH_Lock() 恢复写保护;
  *   7. 回读 memcmp: 与 RAM 副本逐字节比对, 相符才算成功。
  *
  * 【失效安全性: 有安全兜底, 但没有回滚】
  *   本函数只有一页, 没有"先写备份页再切指针"的双备份机制, 因此
  *   擦除之后若在写入途中掉电/复位:
  *     - Flash 里会留下"部分新、部分 0xFF"的半截数据;
  *     - 下次上电 Flash_ReadConfig() 必然因 magic 或 CRC 不匹配而返回 0;
  *     - 上层 App_Init() 于是回落到默认配置, 系统仍能正常启动。
  *   也就是说: 最坏结果是**丢配置**, 而不会**被脏数据带偏**。
  *   代价是用户原来设的定时会全部丢失, 需要重新设置一次。
  *
  * 其它必须知道的副作用
  *   - F1 在擦写 Flash 期间 CPU 取指会被硬件暂停; 本函数跑在 Flash 里,
  *     所以整个过程(擦 1 页约 20~40ms + 200 次半字写)是"卡住"的,
  *     中断虽能触发但中断服务程序也要等取指恢复。因此本函数只允许在
  *     用户确认保存、NTP 校时后等低频时机调用, 绝不可放进主循环。
  */
uint8_t Flash_SaveConfig(const TimerCfg_t *cfg)
{
    TimerCfg_t tmp;
    const uint16_t *src;
    uint16_t n = (uint16_t)(sizeof(TimerCfg_t) / 2);
    uint16_t i;
    uint8_t ok = 1;

    /* 复制到临时区并计算 CRC */
    memcpy(&tmp, cfg, sizeof(TimerCfg_t));
    tmp.crc = Flash_CRC32((const uint8_t *)&tmp, offsetof(TimerCfg_t, crc));
    src = (const uint16_t *)&tmp;

    FLASH_Unlock();

    /* 擦除配置扇区 */
    if (FLASH_ErasePage(FLASH_CFG_ADDR) != FLASH_COMPLETE)
    {
        ok = 0;
    }

    /* 半字编程 */
    if (ok)
    {
        for (i = 0; i < n; i++)
        {
                /* F1 的 Flash 编程接口是"半字(16 位)"粒度, 不能按字节写;
               地址步进 2: 第 i 个半字 -> FLASH_CFG_ADDR + i*2;
               src 是 uint16_t* 且结构体 400 字节为偶数, 因此完全覆盖 200 个半字 */
            if (FLASH_ProgramHalfWord((uint32_t)FLASH_CFG_ADDR + (uint32_t)i * 2u,
                                      src[i]) != FLASH_COMPLETE)
            {
                ok = 0;
                break;
            }
        }
    }

    FLASH_Lock();                           /* 无论擦/写成功与否都必须上锁:
                                               解锁期间一旦有野指针写到 Flash 地址,
                                               就可能误擦程序区, 造成跑飞 */

    /* 回读校验: 把刚写进去的内容再取出来与 RAM 中的副本逐字节比对。
     * 为什么非做不可 —— 半字编程失败时返回码不一定可靠,
     * 真正的判据是"Flash 里到底变成了什么"; 这一比也把坏块、
     * 掉电导致的半截写入一并兜住。 */
    if (ok)
    {
        TimerCfg_t back;
        memcpy(&back, (const uint8_t *)FLASH_CFG_ADDR, sizeof(TimerCfg_t));
        if (memcmp(&back, &tmp, sizeof(TimerCfg_t)) != 0)
        {
            ok = 0;
        }
    }
    return ok;
}
