
// GT9147 电容触摸屏驱动（照 HAL 实验31 gt9147.c / ctiic.c）
// 接线：SCL=PH6、SDA=PI3（软件 I2C）；RST=PI8；INT=PH7；I2C 从机地址 0x14（写 0x28 / 读 0x29）
// 坐标：按 4.3" 800x480 RGB 屏（面板 ID 0x4384）横屏映射（x=原始X、y=原始Y）
// 兼容芯片（照 HAL）：GT911 / GT9147 / GT1158 / GT9271
// 依赖：<cpp/unisym>、<cpp/Device/IIC>、<cpp/Device/GPIO>（由包含方先行引入）
class GT9147_t {
public:
	static constexpr byte MAX_TOUCH = 5;
protected:
	uni::IIC_SOFT& iic;// 软件 I2C（SDA=PI3，SCL=PH6）
	uni::GPIN& rst;    // PI8：复位脚（低有效）
	uni::GPIN& intn;   // PH7：中断脚（本驱动用查询方式，不占中断）
public:
	uint16 x[MAX_TOUCH], y[MAX_TOUCH];// 各触点屏幕坐标（横屏 800x480）
	byte touch_cnt = 0;               // 本次扫描有效点数
	byte dbg_pid[4] = { 0, 0, 0, 0 };// init 读到的产品 ID 原始字节（调试用）
	byte dbg_ack_stage = 0xFF;       // 首个 NACK 阶段：0=写从机地址 1=写reg高 2=写reg低 3=读地址 4=数据；0xFF=全程 ACK 正常
public:
	GT9147_t(uni::IIC_SOFT& _iic, uni::GPIN& _rst, uni::GPIN& _int)
		: iic(_iic), rst(_rst), intn(_int) {}
	bool init();// 复位 + 读产品 ID（GT911/9147/1158/9271）+ 软复位（仅 9147）；成功返回 true
	byte scan();// 返回本次有效点数（0=无触摸），并填充 x[]/y[]
protected:
	bool writeReg(uint16 reg, const byte* buf, byte len);
	bool readReg(uint16 reg, byte* buf, byte len);
};
