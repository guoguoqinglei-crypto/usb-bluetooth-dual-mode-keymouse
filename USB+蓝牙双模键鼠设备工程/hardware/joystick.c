// 引入双摇杆对外接口
#include <joystick.h>
#include <delay.h>

#include <FreeRTOS.h>
#include <task.h>

// ADC 和摇杆供电稳定后再开始校准，避免把上电瞬态记为中点
#define JOY_CALIBRATION_WARMUP_MS 20
// 每个轴在 64ms 内采样 64 次，降低随机噪声对中点的影响
#define JOY_CALIBRATION_SAMPLES 64
#define JOY_CALIBRATION_INTERVAL_MS 1
// 摇杆中心死区，避免中心附近抖动触发方向变化
#define JOY_CENTER_DEADZONE     450
// 一阶低通滤波系数，数值越大响应越快
#define JOY_FILTER_DIVISOR      4

// 坐标方向不旋转
#define JOY_ROTATE_0            0
// 坐标顺时针旋转 90 度：实际向上会修正为代码中的向上
#define JOY_ROTATE_90           1
// 坐标旋转 180 度
#define JOY_ROTATE_180          2
// 坐标逆时针旋转 90 度
#define JOY_ROTATE_270          3

// 左摇杆 X 轴向右时 ADC 增大
#define JOYSTICK_LX_RIGHT_HIGH  1
// 右摇杆 X 轴向右时 ADC 增大
#define JOYSTICK_RX_RIGHT_HIGH  1
// 左摇杆 Y 轴向上时 ADC 增大
#define JOYSTICK_LY_UP_HIGH     1
// 右摇杆 Y 轴向上时 ADC 增大
#define JOYSTICK_RY_UP_HIGH     1

// 当前实测左摇杆向上被识别成向左，因此顺时针旋转 90 度。
//       两边必须保持一致，否则鼠标会出现"推上却往左移"。
#define JOYSTICK_LEFT_ROTATE    JOY_ROTATE_90
// 当前实测右摇杆按同样方式修正
#define JOYSTICK_RIGHT_ROTATE   JOY_ROTATE_90

// 保存四个轴的上电中位值
static uint16_t joystickCenter[4] = {2048, 2048, 2048, 2048};

static void Joystick_CalibrateCenter(void);
/* ============================================================================
 * 静止时自动重新标定中位
 * ----------------------------------------------------------------------------
 * 问题现象：不碰摇杆，鼠标光标也会自己往一个方向慢慢跑（蓝牙/USB 都有）。
 * 原因：上电时 Joystick_CalibrateCenter() 采的 64 个样本里如果摇杆没完全静止，
 *       或者电位器静置后的实际中位与上电瞬间不同，映射出来的静止值就会
 *       落在鼠标死区（6000）之外 —— 每次报告都发 ±1，光标就持续移动。
 *
 * 做法：只在"轴处于死区内（说明没被推动）"时，缓慢把中位拉向当前读数，
 *       每次修正 1/8，约 100ms 一次。推杆过程中（超出死区）绝不修改中位。
 * 调用约定：由 1ms 周期的任务调用。
 * ========================================================================== */
#define JOY_RECENTER_CALLS      100U    /* 调用 100 次 ≈ 100ms */
#define JOY_RECENTER_WINDOW     9000    /* 与鼠标死区一致：死区内视为静止 */
#define JOY_RECENTER_SHIFT      3U      /* 每次修正 1/8，避免中位跳动 */

static uint16_t s_recenterCount = 0U;

/* 模式切换后的快速校准只允许采集静止摇杆。
 * 若正在推杆，均值相对旧中位会明显偏移，采样最大/最小值也会拉大；
 * 两种情况下都保留旧中位，等松手后由 AutoRecenter 缓慢修正。 */
#define JOY_RECALIBRATE_SAMPLES     16U
#define JOY_RECALIBRATE_MAX_SPREAD  128U
#define JOY_RECALIBRATE_MAX_OFFSET  6000

void Joystick_RecalibrateNow(void)
{
    uint8_t  axis;
    uint8_t  i;
    uint16_t raw;
    uint16_t minRaw;
    uint16_t maxRaw;
    uint16_t mean;
    uint32_t sum;
    int16_t  mapped;

    for (axis = 0U; axis < 4U; axis++)
    {
        sum = 0U;
        minRaw = 0xFFFFU;
        maxRaw = 0U;

        for (i = 0U; i < JOY_RECALIBRATE_SAMPLES; i++)
        {
            raw = Joystick_ReadAxis(axis);
            sum += raw;
            if (raw < minRaw)
            {
                minRaw = raw;
            }
            if (raw > maxRaw)
            {
                maxRaw = raw;
            }
        }

        mean = (uint16_t)((sum + (JOY_RECALIBRATE_SAMPLES / 2U)) /
                          JOY_RECALIBRATE_SAMPLES);
        mapped = Joystick_MapRawValue(axis, mean);

        if (((uint16_t)(maxRaw - minRaw) > JOY_RECALIBRATE_MAX_SPREAD) ||
            (mapped > JOY_RECALIBRATE_MAX_OFFSET) ||
            (mapped < -JOY_RECALIBRATE_MAX_OFFSET))
        {
            continue;               /* 正在推杆或采样不稳定：绝不覆盖中位 */
        }
        joystickCenter[axis] = mean;
    }
}

void Joystick_AutoRecenter(void)
{
    uint8_t  axis;
    int16_t  mapped;
    uint16_t raw;
    int32_t  diff;

    s_recenterCount++;
    if (s_recenterCount < JOY_RECENTER_CALLS)
    {
        return;
    }
    s_recenterCount = 0U;

    for (axis = 0U; axis < 4U; axis++)
    {
        raw    = Joystick_ReadAxis(axis);
        mapped = Joystick_MapRawValue(axis, raw);

        if ((mapped > JOY_RECENTER_WINDOW) || (mapped < -JOY_RECENTER_WINDOW))
        {
            continue;                   /* 推杆中：不动中位 */
        }

        diff = (int32_t)raw - (int32_t)joystickCenter[axis];
        joystickCenter[axis] = (uint16_t)((int32_t)joystickCenter[axis]
                                          + (diff >> JOY_RECENTER_SHIFT));
    }
}

static int32_t Joystick_Abs32(int32_t value);
static int32_t Joystick_GetSignedX(const Joystick_State *state);
static int32_t Joystick_GetSignedUp(const Joystick_State *state);
static void Joystick_GetRotatedComponents(const Joystick_State *state,
                                          int32_t *dx,
                                          int32_t *dy);
static void Joystick_UpdateDerivedState(Joystick_State *state);

static uint8_t Joystick_GetADCChannel(uint8_t axis)
{
    // 第 1 步：根据逻辑轴编号选择 ADC 通道
    switch (axis)
    {
        case JOYSTICK_LX: return ADC_Channel_4;   /* PA4 */
        case JOYSTICK_LY: return ADC_Channel_5;   /* PA5 */
        case JOYSTICK_RX: return ADC_Channel_6;   /* PA6 */
        case JOYSTICK_RY: return ADC_Channel_7;   /* PA7 */
        default:          return ADC_Channel_4;
    }
}

void Joystick_Init(void)
{
    // 第 1 步：开启 GPIOA、GPIOB 和 ADC1 时钟
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    // 第 2 步：把 PA4~PA7 配置为模拟输入
    /* PA4~PA7 配置为模拟输入 */
    GPIOA->CRL &= ~(GPIO_CRL_MODE4 | GPIO_CRL_CNF4);
    GPIOA->CRL &= ~(GPIO_CRL_MODE5 | GPIO_CRL_CNF5);
    GPIOA->CRL &= ~(GPIO_CRL_MODE6 | GPIO_CRL_CNF6);
    GPIOA->CRL &= ~(GPIO_CRL_MODE7 | GPIO_CRL_CNF7);

    // 第 3 步：把 PB8、PB9 配置为摇杆按下按键
    /* PB8/PB9 为摇杆按下按键，低电平有效 */
    GPIOB->CRH &= ~(GPIO_CRH_MODE8 | GPIO_CRH_CNF8);
    GPIOB->CRH |= GPIO_CRH_CNF8_1;
    GPIOB->ODR |= GPIO_ODR_ODR8;

    GPIOB->CRH &= ~(GPIO_CRH_MODE9 | GPIO_CRH_CNF9);
    GPIOB->CRH |= GPIO_CRH_CNF9_1;
    GPIOB->ODR |= GPIO_ODR_ODR9;

    // 第 4 步：配置 ADC 时钟为 PCLK2 的 6 分频
    /* ADC 时钟：PCLK2 / 6 = 12MHz */
    RCC->CFGR |= RCC_CFGR_ADCPRE_1;
    RCC->CFGR &= ~RCC_CFGR_ADCPRE_0;

    // 第 5 步：复位 ADC 控制寄存器并打开 ADC
    ADC1->CR1 = 0;
    ADC1->CR2 = ADC_CR2_ADON;

    // 第 6 步：执行 ADC 复位校准
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while (ADC1->CR2 & ADC_CR2_RSTCAL);

    // 第 7 步：执行 ADC 校准
    ADC1->CR2 |= ADC_CR2_CAL;
    while (ADC1->CR2 & ADC_CR2_CAL);

    // 第 8 步：采集并保存四个轴的中间位置
    Joystick_CalibrateCenter();
}

uint16_t Joystick_ReadAxis(uint8_t axis)
{
    // 第 1 步：取得轴对应的 ADC 通道
    uint8_t channel = Joystick_GetADCChannel(axis);
    uint16_t value;
    uint8_t rtosRunning = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1U : 0U;

    /* ADC1 的通道选择、EOC 标志和 DR 都是全局共享资源。
     * FreeRTOS 下 USB / BT 两个任务可能同时进入这里：若另一任务在中途
     * 读走 DR，EOC 被清除，本任务就可能永久卡在下面的等待循环。
     * 单次转换约 21us，用短临界区保证一次转换完整、原子地完成。 */
    if (rtosRunning != 0U)
    {
        taskENTER_CRITICAL();
    }

    // 第 2 步：设置规则组只有一个转换通道
    ADC1->SQR1 &= ~ADC_SQR1_L;
    // 第 3 步：设置当前转换通道
    ADC1->SQR3 = channel;

    // 第 4 步：配置采样时间为 239.5 个 ADC 周期
    if (channel <= 9)
    {
        ADC1->SMPR2 &= ~(0x07U << (channel * 3));
        ADC1->SMPR2 |= (0x07U << (channel * 3));
    }
    else
    {
        ADC1->SMPR1 &= ~(0x07U << ((channel - 10) * 3));
        ADC1->SMPR1 |= (0x07U << ((channel - 10) * 3));
    }

    // 第 5 步：确保 ADC 已开启
    ADC1->CR2 |= ADC_CR2_ADON;
    // 第 6 步：启动软件转换
    ADC1->CR2 |= ADC_CR2_SWSTART;
    // 第 7 步：等待转换结束
    while (!(ADC1->SR & ADC_SR_EOC));

    // 第 8 步：读取 12 位转换结果
    value = (uint16_t)ADC1->DR;

    if (rtosRunning != 0U)
    {
        taskEXIT_CRITICAL();
    }
    return value;
}

int16_t Joystick_MapRawValue(uint8_t axis, uint16_t rawValue)
{
    int32_t delta;
    int32_t range;
    int32_t mapped;

    // 第 1 步：计算相对校准中点的偏移
    delta = (int32_t)rawValue - (int32_t)joystickCenter[axis];

    // 第 2 步：根据偏移方向选择可用满量程范围
    if (delta >= 0)
    {
        range = 4095 - (int32_t)joystickCenter[axis];
    }
    else
    {
        range = (int32_t)joystickCenter[axis];
    }

    // 第 3 步：避免中点为边界时除零
    if (range <= 0)
    {
        return 0;
    }

    // 第 4 步：映射到 HID 16 位有符号范围
    mapped = (delta * 32767) / range;
    if (mapped > 32767)
    {
        mapped = 32767;
    }
    else if (mapped < -32767)
    {
        mapped = -32767;
    }

    return (int16_t)mapped;
}

static void Joystick_CalibrateCenter(void)
{
    // 轴编号、采样编号和累加值
    uint8_t axis;
    uint8_t sample;
    uint32_t sum;

    // 第 1 步：等待 ADC、参考电压和摇杆供电稳定
    Delay_ms(JOY_CALIBRATION_WARMUP_MS);

    // 第 2 步：依次校准四个轴，校准期间保持摇杆释放
    for (axis = JOYSTICK_LX; axis <= JOYSTICK_RY; axis++)
    {
        sum = 0;
        // 第 3 步：跨一段时间采样，而不是只取上电瞬间的十几个值
        for (sample = 0; sample < JOY_CALIBRATION_SAMPLES; sample++)
        {
            sum += Joystick_ReadAxis(axis);
            Delay_ms(JOY_CALIBRATION_INTERVAL_MS);
        }
        // 第 4 步：四舍五入取平均，减少整数截断造成的固定方向偏差
        joystickCenter[axis] = (uint16_t)((sum + (JOY_CALIBRATION_SAMPLES / 2U))
                                         / JOY_CALIBRATION_SAMPLES);
    }
}

static int32_t Joystick_Abs32(int32_t value)
{
    // 返回有符号数的绝对值
    return (value < 0) ? -value : value;
}

static int32_t Joystick_GetSignedX(const Joystick_State *state)
{
    // 第 1 步：计算 X 轴相对中心的偏移
    int32_t delta = (int32_t)state->x - (int32_t)joystickCenter[state->xAxis];

    // 第 2 步：按实际安装方向修正符号
    if (state->xAxis == JOYSTICK_LX)
    {
        return JOYSTICK_LX_RIGHT_HIGH ? delta : -delta;
    }

    return JOYSTICK_RX_RIGHT_HIGH ? delta : -delta;
}

static int32_t Joystick_GetSignedUp(const Joystick_State *state)
{
    // 第 1 步：计算 Y 轴相对中心的偏移
    int32_t delta = (int32_t)state->y - (int32_t)joystickCenter[state->yAxis];

    // 第 2 步：按实际安装方向修正符号
    if (state->yAxis == JOYSTICK_LY)
    {
        return JOYSTICK_LY_UP_HIGH ? delta : -delta;
    }

    return JOYSTICK_RY_UP_HIGH ? delta : -delta;
}

static void Joystick_GetRotatedComponents(const Joystick_State *state,
                                          int32_t *dx,
                                          int32_t *dy)
{
    // 第 1 步：读取硬件原始 X/Y 方向
    int32_t rawX = Joystick_GetSignedX(state);
    int32_t rawY = Joystick_GetSignedUp(state);
    // 第 2 步：根据摇杆编号选择旋转配置
    uint8_t rotation = (state->xAxis == JOYSTICK_LX)
                       ? JOYSTICK_LEFT_ROTATE
                       : JOYSTICK_RIGHT_ROTATE;

    // 第 3 步：按配置修正坐标方向
    switch (rotation)
    {
        case JOY_ROTATE_90:
            *dx = rawY;
            *dy = -rawX;
            break;

        case JOY_ROTATE_180:
            *dx = -rawX;
            *dy = -rawY;
            break;

        case JOY_ROTATE_270:
            *dx = -rawY;
            *dy = rawX;
            break;

        case JOY_ROTATE_0:
        default:
            *dx = rawX;
            *dy = rawY;
            break;
    }
}

static void Joystick_UpdateDerivedState(Joystick_State *state)
{
    // 第 1 步：取得旋转修正后的 X/Y 偏移
    int32_t dx;
    int32_t dy;
    Joystick_GetRotatedComponents(state, &dx, &dy);
    // 第 2 步：计算 X/Y 偏移的绝对值
    int32_t absX = Joystick_Abs32(dx);
    int32_t absY = Joystick_Abs32(dy);
    int32_t maxUp;

    // 第 3 步：判断是否位于中心死区
    if ((absX < JOY_CENTER_DEADZONE) && (absY < JOY_CENTER_DEADZONE))
    {
        state->direction = JOY_DIR_CENTER;
    }
    // 第 4 步：判断是否为水平方向
    else if (absY * 100 < absX * 41)
    {
        state->direction = (dx > 0) ? JOY_DIR_RIGHT : JOY_DIR_LEFT;
    }
    // 第 5 步：判断是否为竖直方向
    else if (absX * 100 < absY * 41)
    {
        state->direction = (dy > 0) ? JOY_DIR_UP : JOY_DIR_DOWN;
    }
    // 第 6 步：其余情况判断为斜向
    else if (dx > 0)
    {
        state->direction = (dy > 0) ? JOY_DIR_UP_RIGHT : JOY_DIR_DOWN_RIGHT;
    }
    else
    {
        state->direction = (dy > 0) ? JOY_DIR_UP_LEFT : JOY_DIR_DOWN_LEFT;
    }

    // 第 7 步：如果没有向上分量，则亮度为 0
    if (dy <= 0)
    {
        state->upBrightness = 0;
        return;
    }

    // 第 8 步：计算该 Y 轴向上满量程时的最大偏移
    if (state->yAxis == JOYSTICK_LY)
    {
        maxUp = JOYSTICK_LY_UP_HIGH ? (4095 - joystickCenter[state->yAxis])
                                    : joystickCenter[state->yAxis];
    }
    else
    {
        maxUp = JOYSTICK_RY_UP_HIGH ? (4095 - joystickCenter[state->yAxis])
                                    : joystickCenter[state->yAxis];
    }

    // 第 9 步：把向上偏移换算为 0~100 的亮度
    if (maxUp <= 0)
    {
        state->upBrightness = 0;
    }
    else if (dy >= maxUp)
    {
        state->upBrightness = 100;
    }
    else
    {
        state->upBrightness = (uint8_t)((dy * 100) / maxUp);
    }
}

void Joystick_StateInit(Joystick_State *state, uint8_t xAxis, uint8_t yAxis)
{
    // 第 1 步：保存轴编号
    state->xAxis = xAxis;
    state->yAxis = yAxis;
    // 第 2 步：读取当前 X/Y 原始值
    state->x = Joystick_ReadAxis(xAxis);
    state->y = Joystick_ReadAxis(yAxis);
    // 第 3 步：标记状态已经初始化
    state->initialized = 1;
    // 第 4 步：计算初始方向和亮度
    Joystick_UpdateDerivedState(state);
}

void Joystick_Update(Joystick_State *state)
{
    // 第 1 步：读取 X/Y 原始 ADC 值
    uint16_t rawX = Joystick_ReadAxis(state->xAxis);
    uint16_t rawY = Joystick_ReadAxis(state->yAxis);

    // 第 2 步：首次采样直接使用原始值
    if (!state->initialized)
    {
        state->x = rawX;
        state->y = rawY;
        state->initialized = 1;
    }
    // 第 3 步：后续采样使用一阶低通滤波
    else
    {
        state->x = (uint16_t)(((uint32_t)state->x * (JOY_FILTER_DIVISOR - 1) + rawX)
                              / JOY_FILTER_DIVISOR);
        state->y = (uint16_t)(((uint32_t)state->y * (JOY_FILTER_DIVISOR - 1) + rawY)
                              / JOY_FILTER_DIVISOR);
    }

    // 第 4 步：重新计算方向和向上亮度
    Joystick_UpdateDerivedState(state);
}

uint8_t Joystick_GetKeyState(uint8_t key)
{
    // 第 1 步：根据按键编号读取 GPIO 电平
    switch (key)
    {
        case JOYSTICK_L3:
            return (GPIOB->IDR & GPIO_IDR_IDR9) ? 1 : 0;

        case JOYSTICK_R3:
            return (GPIOB->IDR & GPIO_IDR_IDR8) ? 1 : 0;

        default:
            return 1;
    }
}

/* ============================================================================
 * L3 / R3 独立按键状态机
 * ----------------------------------------------------------------------------
 * IDLE -> PRESS_DEBOUNCE -> PRESSED -> RELEASE_DEBOUNCE -> IDLE
 * 每个状态只属于一个摇杆按键，两个按键可同时按下、互不清除对方状态。
 * 10ms 扫描一次，连续 2 个周期电平一致才确认边沿，即 20ms 消抖。
 * ========================================================================== */
#define JOYKEY_STATE_IDLE              0U
#define JOYKEY_STATE_PRESS_DEBOUNCE    1U
#define JOYKEY_STATE_PRESSED           2U
#define JOYKEY_STATE_RELEASE_DEBOUNCE  3U
#define JOYKEY_DEBOUNCE_COUNT          2U
#define JOYKEY_COUNT                   2U

typedef struct
{
    uint8_t state;
    uint8_t count;
} Joystick_KeyFsm;

static Joystick_KeyFsm s_joyKeyFsm[JOYKEY_COUNT];

void Joystick_KeyInit(void)
{
    uint8_t key;

    for (key = 0U; key < JOYKEY_COUNT; key++)
    {
        s_joyKeyFsm[key].state = JOYKEY_STATE_IDLE;
        s_joyKeyFsm[key].count = 0U;
    }
}

void Joystick_KeyUpdate(void)
{
    uint8_t key;

    for (key = 0U; key < JOYKEY_COUNT; key++)
    {
        const uint8_t pressed = (Joystick_GetKeyState(key) == 0U) ? 1U : 0U;
        Joystick_KeyFsm *fsm = &s_joyKeyFsm[key];

        switch (fsm->state)
        {
            case JOYKEY_STATE_IDLE:
                if (pressed != 0U)
                {
                    fsm->state = JOYKEY_STATE_PRESS_DEBOUNCE;
                    fsm->count = 0U;
                }
                break;

            case JOYKEY_STATE_PRESS_DEBOUNCE:
                if (pressed == 0U)
                {
                    fsm->state = JOYKEY_STATE_IDLE;  /* 抖动，不产生按下 */
                }
                else if (++fsm->count >= JOYKEY_DEBOUNCE_COUNT)
                {
                    fsm->state = JOYKEY_STATE_PRESSED;
                    fsm->count = 0U;
                }
                break;

            case JOYKEY_STATE_PRESSED:
                if (pressed == 0U)
                {
                    fsm->state = JOYKEY_STATE_RELEASE_DEBOUNCE;
                    fsm->count = 0U;
                }
                break;

            case JOYKEY_STATE_RELEASE_DEBOUNCE:
                if (pressed != 0U)
                {
                    fsm->state = JOYKEY_STATE_PRESSED; /* 释放抖动，仍保持按下 */
                }
                else if (++fsm->count >= JOYKEY_DEBOUNCE_COUNT)
                {
                    fsm->state = JOYKEY_STATE_IDLE;
                    fsm->count = 0U;
                }
                break;

            default:
                fsm->state = JOYKEY_STATE_IDLE;
                fsm->count = 0U;
                break;
        }
    }
}

uint8_t Joystick_KeyIsDown(uint8_t key)
{
    if ((key >= JOYKEY_COUNT) ||
        ((s_joyKeyFsm[key].state != JOYKEY_STATE_PRESSED) &&
         (s_joyKeyFsm[key].state != JOYKEY_STATE_RELEASE_DEBOUNCE)))
    {
        return 0U;
    }
    return 1U;
}
