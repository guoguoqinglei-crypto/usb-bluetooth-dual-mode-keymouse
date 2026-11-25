#include <key.h>
#include <delay.h>

static Key_Callback key_callback = 0;

// 按键初始化
void Key_Init(void)
{
    // 开启时钟
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;   // 开启GPIOC时钟
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;   // 开启GPIOB时钟
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;   // 开启AFIO时钟

    // 配置PC4, PC5为上拉输入模式
    GPIOC->CRL &= ~GPIO_CRL_MODE4;
    GPIOC->CRL &= ~GPIO_CRL_CNF4;
    GPIOC->CRL |= GPIO_CRL_CNF4_1;
    GPIOC->ODR |= GPIO_ODR_ODR4;

    GPIOC->CRL &= ~GPIO_CRL_MODE5;
    GPIOC->CRL &= ~GPIO_CRL_CNF5;
    GPIOC->CRL |= GPIO_CRL_CNF5_1;
    GPIOC->ODR |= GPIO_ODR_ODR5;

    // 配置PB0, PB1, PB2为上拉输入模式
    GPIOB->CRL &= ~GPIO_CRL_MODE0;
    GPIOB->CRL &= ~GPIO_CRL_CNF0;
    GPIOB->CRL |= GPIO_CRL_CNF0_1;
    GPIOB->ODR |= GPIO_ODR_ODR0;

    GPIOB->CRL &= ~GPIO_CRL_MODE1;
    GPIOB->CRL &= ~GPIO_CRL_CNF1;
    GPIOB->CRL |= GPIO_CRL_CNF1_1;
    GPIOB->ODR |= GPIO_ODR_ODR1;

    GPIOB->CRL &= ~GPIO_CRL_MODE2;
    GPIOB->CRL &= ~GPIO_CRL_CNF2;
    GPIOB->CRL |= GPIO_CRL_CNF2_1;
    GPIOB->ODR |= GPIO_ODR_ODR2;

    // 配置PB10, PB11, PB14为上拉输入模式
    GPIOB->CRH &= ~GPIO_CRH_MODE10;
    GPIOB->CRH &= ~GPIO_CRH_CNF10;
    GPIOB->CRH |= GPIO_CRH_CNF10_1;
    GPIOB->ODR |= GPIO_ODR_ODR10;

    GPIOB->CRH &= ~GPIO_CRH_MODE11;
    GPIOB->CRH &= ~GPIO_CRH_CNF11;
    GPIOB->CRH |= GPIO_CRH_CNF11_1;
    GPIOB->ODR |= GPIO_ODR_ODR11;

    GPIOB->CRH &= ~GPIO_CRH_MODE14;
    GPIOB->CRH &= ~GPIO_CRH_CNF14;
    GPIOB->CRH |= GPIO_CRH_CNF14_1;
    GPIOB->ODR |= GPIO_ODR_ODR14;
}

// 按键中断初始化
void Key_Interrupt_Init(void)
{
    // 全局统一为 3 bit 抢占优先级、1 bit 子优先级
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    // 1. 开启时钟
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;

    // 2. 配置GPIO为上拉输入模式
    // PC4 - A键
    GPIOC->CRL &= ~GPIO_CRL_MODE4;
    GPIOC->CRL &= ~GPIO_CRL_CNF4;
    GPIOC->CRL |= GPIO_CRL_CNF4_1;
    GPIOC->ODR |= GPIO_ODR_ODR4;

    // PC5 - B键
    GPIOC->CRL &= ~GPIO_CRL_MODE5;
    GPIOC->CRL &= ~GPIO_CRL_CNF5;
    GPIOC->CRL |= GPIO_CRL_CNF5_1;
    GPIOC->ODR |= GPIO_ODR_ODR5;

    // PB0 - X键
    GPIOB->CRL &= ~GPIO_CRL_MODE0;
    GPIOB->CRL &= ~GPIO_CRL_CNF0;
    GPIOB->CRL |= GPIO_CRL_CNF0_1;
    GPIOB->ODR |= GPIO_ODR_ODR0;

    // PB1 - Y键
    GPIOB->CRL &= ~GPIO_CRL_MODE1;
    GPIOB->CRL &= ~GPIO_CRL_CNF1;
    GPIOB->CRL |= GPIO_CRL_CNF1_1;
    GPIOB->ODR |= GPIO_ODR_ODR1;

    // PB2 - UP键
    GPIOB->CRL &= ~GPIO_CRL_MODE2;
    GPIOB->CRL &= ~GPIO_CRL_CNF2;
    GPIOB->CRL |= GPIO_CRL_CNF2_1;
    GPIOB->ODR |= GPIO_ODR_ODR2;

    // PB10 - DN键
    GPIOB->CRH &= ~GPIO_CRH_MODE10;
    GPIOB->CRH &= ~GPIO_CRH_CNF10;
    GPIOB->CRH |= GPIO_CRH_CNF10_1;
    GPIOB->ODR |= GPIO_ODR_ODR10;

    // PB11 - LF键
    GPIOB->CRH &= ~GPIO_CRH_MODE11;
    GPIOB->CRH &= ~GPIO_CRH_CNF11;
    GPIOB->CRH |= GPIO_CRH_CNF11_1;
    GPIOB->ODR |= GPIO_ODR_ODR11;

    // PB14 - RG键
    GPIOB->CRH &= ~GPIO_CRH_MODE14;
    GPIOB->CRH &= ~GPIO_CRH_CNF14;
    GPIOB->CRH |= GPIO_CRH_CNF14_1;
    GPIOB->ODR |= GPIO_ODR_ODR14;

    // 3. 配置AFIO - 映射外部中断线
    // EXTI0 - PB0 (X键)
    AFIO->EXTICR[0] &= ~(0xF << 0);
    AFIO->EXTICR[0] |= (0x1 << 0);

    // EXTI1 - PB1 (Y键)
    AFIO->EXTICR[0] &= ~(0xF << 4);
    AFIO->EXTICR[0] |= (0x1 << 4);

    // EXTI2 - PB2 (UP键)
    AFIO->EXTICR[0] &= ~(0xF << 8);
    AFIO->EXTICR[0] |= (0x1 << 8);

    // EXTI4 - PC4 (A键)
    AFIO->EXTICR[1] &= ~(0xF << 0);
    AFIO->EXTICR[1] |= (0x2 << 0);

    // EXTI5 - PC5 (B键)
    AFIO->EXTICR[1] &= ~(0xF << 4);
    AFIO->EXTICR[1] |= (0x2 << 4);

    // EXTI10 - PB10 (DN键)
    AFIO->EXTICR[2] &= ~(0xF << 8);
    AFIO->EXTICR[2] |= (0x1 << 8);

    // EXTI11 - PB11 (LF键)
    AFIO->EXTICR[2] &= ~(0xF << 12);
    AFIO->EXTICR[2] |= (0x1 << 12);

    // EXTI14 - PB14 (RG键)
    AFIO->EXTICR[3] &= ~(0xF << 8);
    AFIO->EXTICR[3] |= (0x1 << 8);

    // 4. 配置EXTI - 下降沿触发
    EXTI->IMR |= (EXTI_Line0 | EXTI_Line1 | EXTI_Line2 |
                  EXTI_Line4 | EXTI_Line5 |
                  EXTI_Line10 | EXTI_Line11 | EXTI_Line14);

    EXTI->FTSR |= (EXTI_Line0 | EXTI_Line1 | EXTI_Line2 |
                   EXTI_Line4 | EXTI_Line5 |
                   EXTI_Line10 | EXTI_Line11 | EXTI_Line14);

    // 5. 配置NVIC
    NVIC_InitTypeDef NVIC_InitStructure;

    // EXTI0 - X键
    NVIC_InitStructure.NVIC_IRQChannel = EXTI0_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    // EXTI1 - Y键
    NVIC_InitStructure.NVIC_IRQChannel = EXTI1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_Init(&NVIC_InitStructure);

    // EXTI2 - UP键
    NVIC_InitStructure.NVIC_IRQChannel = EXTI2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_Init(&NVIC_InitStructure);

    // EXTI4 - A键
    NVIC_InitStructure.NVIC_IRQChannel = EXTI4_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_Init(&NVIC_InitStructure);

    // EXTI9_5 - B键
    NVIC_InitStructure.NVIC_IRQChannel = EXTI9_5_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_Init(&NVIC_InitStructure);

    // EXTI15_10 - DN, LF, RG键
    NVIC_InitStructure.NVIC_IRQChannel = EXTI15_10_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_Init(&NVIC_InitStructure);
}

// 设置按键回调函数
void Key_SetCallback(Key_Callback callback)
{
    key_callback = callback;
}

// 按键扫描函数
uint8_t Key_Scan(uint8_t mode)
{
    static uint8_t key_up = 1;

    if (mode)
        key_up = 1;

    if (key_up && (
        (GPIOC->IDR & GPIO_IDR_IDR4) == 0 ||   // A键
        (GPIOC->IDR & GPIO_IDR_IDR5) == 0 ||   // B键
        (GPIOB->IDR & GPIO_IDR_IDR0) == 0 ||   // X键
        (GPIOB->IDR & GPIO_IDR_IDR1) == 0 ||   // Y键
        (GPIOB->IDR & GPIO_IDR_IDR2) == 0 ||   // UP键
        (GPIOB->IDR & GPIO_IDR_IDR10) == 0 ||  // DN键
        (GPIOB->IDR & GPIO_IDR_IDR11) == 0 ||  // LF键
        (GPIOB->IDR & GPIO_IDR_IDR14) == 0))   // RG键
    {
        Delay_ms(10);
        key_up = 0;

        if ((GPIOC->IDR & GPIO_IDR_IDR4) == 0)       return KEY_A;
        else if ((GPIOC->IDR & GPIO_IDR_IDR5) == 0)  return KEY_B;
        else if ((GPIOB->IDR & GPIO_IDR_IDR0) == 0)  return KEY_X;
        else if ((GPIOB->IDR & GPIO_IDR_IDR1) == 0)  return KEY_Y;
        else if ((GPIOB->IDR & GPIO_IDR_IDR2) == 0)  return KEY_UP;
        else if ((GPIOB->IDR & GPIO_IDR_IDR10) == 0) return KEY_DN;
        else if ((GPIOB->IDR & GPIO_IDR_IDR11) == 0) return KEY_LF;
        else if ((GPIOB->IDR & GPIO_IDR_IDR14) == 0) return KEY_RG;
    }
    else if (
        (GPIOC->IDR & GPIO_IDR_IDR4) &&
        (GPIOC->IDR & GPIO_IDR_IDR5) &&
        (GPIOB->IDR & GPIO_IDR_IDR0) &&
        (GPIOB->IDR & GPIO_IDR_IDR1) &&
        (GPIOB->IDR & GPIO_IDR_IDR2) &&
        (GPIOB->IDR & GPIO_IDR_IDR10) &&
        (GPIOB->IDR & GPIO_IDR_IDR11) &&
        (GPIOB->IDR & GPIO_IDR_IDR14))
    {
        key_up = 1;
    }

    return 0;
}

// 获取指定按键状态
uint8_t Key_GetState(uint8_t key)
{
    switch (key)
    {
        case KEY_A:  return (GPIOC->IDR & GPIO_IDR_IDR4) ? 1 : 0;
        case KEY_B:  return (GPIOC->IDR & GPIO_IDR_IDR5) ? 1 : 0;
        case KEY_X:  return (GPIOB->IDR & GPIO_IDR_IDR0) ? 1 : 0;
        case KEY_Y:  return (GPIOB->IDR & GPIO_IDR_IDR1) ? 1 : 0;
        case KEY_UP: return (GPIOB->IDR & GPIO_IDR_IDR2) ? 1 : 0;
        case KEY_DN: return (GPIOB->IDR & GPIO_IDR_IDR10) ? 1 : 0;
        case KEY_LF: return (GPIOB->IDR & GPIO_IDR_IDR11) ? 1 : 0;
        case KEY_RG: return (GPIOB->IDR & GPIO_IDR_IDR14) ? 1 : 0;
        default:     return 1;
    }
}

// 中断服务函数 - X键 (EXTI0)
void EXTI0_IRQHandler(void)
{
    if (EXTI->PR & EXTI_Line0)
    {
        EXTI->PR = EXTI_Line0;
        if (key_callback) key_callback(KEY_X);
    }
}

// 中断服务函数 - Y键 (EXTI1)
void EXTI1_IRQHandler(void)
{
    if (EXTI->PR & EXTI_Line1)
    {
        EXTI->PR = EXTI_Line1;
        if (key_callback) key_callback(KEY_Y);
    }
}

// 中断服务函数 - UP键 (EXTI2)
void EXTI2_IRQHandler(void)
{
    if (EXTI->PR & EXTI_Line2)
    {
        EXTI->PR = EXTI_Line2;
        if (key_callback) key_callback(KEY_UP);
    }
}

// 中断服务函数 - A键 (EXTI4)
void EXTI4_IRQHandler(void)
{
    if (EXTI->PR & EXTI_Line4)
    {
        EXTI->PR = EXTI_Line4;
        if (key_callback) key_callback(KEY_A);
    }
}

// 中断服务函数 - B键 (EXTI5)
void EXTI9_5_IRQHandler(void)
{
    if (EXTI->PR & EXTI_Line5)
    {
        EXTI->PR = EXTI_Line5;
        if (key_callback) key_callback(KEY_B);
    }
}

void EXTI15_10_IRQHandler(void)
{
    if (EXTI->PR & EXTI_Line10)
    {
        EXTI->PR = EXTI_Line10;
        if (key_callback) key_callback(KEY_DN);
    }

    if (EXTI->PR & EXTI_Line11)
    {
        EXTI->PR = EXTI_Line11;
        if (key_callback) key_callback(KEY_LF);
    }

    if (EXTI->PR & EXTI_Line14)
    {
        EXTI->PR = EXTI_Line14;
        if (key_callback) key_callback(KEY_RG);
    }
}

/* ========== 按键状态机 (FSM) 实现 ========== */

static KeyFSM_TypeDef keyFsm[9];

// 初始化所有按键状态机
void Key_FSM_Init(void)
{
    // 遍历 9 个状态机实例（包含保留的索引 0）
    for (uint8_t i = 0; i < 9; i++)
    {
        keyFsm[i].state = KEY_STATE_IDLE;  // 初始状态：空闲，等待按键按下
        keyFsm[i].cnt   = 0;                // 消抖/长按计数器清零
        keyFsm[i].event = KEY_EVENT_NONE;   // 无事件产生
    }
}

void Key_FSM_Update(void)
{
    // 依次处理 8 个按键：KEY_A(1) ~ KEY_RG(8)
    for (uint8_t k = KEY_A; k <= KEY_RG; k++)
    {
        // 第1步：读取当前按键电平
        // Key_GetState 返回 1 表示释放（高电平），0 表示按下（低电平）
        // 这里转换为 pressed：1 = 按下，0 = 释放，便于状态机判断
        uint8_t pressed = (Key_GetState(k) == 0) ? 1 : 0;

        // 第2步：每轮更新先将事件清空，事件只在特定状态转换时产生
        keyFsm[k].event = KEY_EVENT_NONE;

        // 第3步：根据当前状态决定下一步动作
        switch (keyFsm[k].state)
        {
            // ----------------------------
            // 状态 0：IDLE（空闲/未按下）
            // ----------------------------
            case KEY_STATE_IDLE:
                // 如果检测到低电平，认为按键可能开始按下
                if (pressed)
                {
                    // 进入消抖状态，同时清零计数器，开始统计稳定低电平次数
                    keyFsm[k].state = KEY_STATE_DEBOUNCE;
                    keyFsm[k].cnt   = 0;
                }
                // 若仍为高电平，保持 IDLE，不产生事件
                break;

            // ----------------------------
            // 状态 1：DEBOUNCE（按下消抖）
            // ----------------------------
            case KEY_STATE_DEBOUNCE:
                // 消抖期间仍然检测到低电平，说明不是抖动
                if (pressed)
                {
                    // 计数器加 1，代表又经历了一个扫描周期仍为低电平
                    keyFsm[k].cnt++;

                    // 当连续低电平次数达到消抖阈值，确认一次有效按下
                    if (keyFsm[k].cnt >= KEY_FSM_DEBOUNCE_CNT)
                    {
                        // 切换到已按下状态
                        keyFsm[k].state = KEY_STATE_PRESSED;
                        // 清零计数器，为后续长按计时做准备
                        keyFsm[k].cnt   = 0;
                        // 注意：此时还不产生短按事件，避免按住期间误触发短按
                        // 短按事件将在释放时产生（若未达到长按阈值）
                    }
                }
                // 消抖期间电平变回高，说明是抖动或按键已松开
                else
                {
                    // 回到空闲状态，不报告任何事件
                    keyFsm[k].state = KEY_STATE_IDLE;
                }
                break;

            // ----------------------------
            // 状态 2：PRESSED（已确认按下）
            // ----------------------------
            case KEY_STATE_PRESSED:
                // 按键仍然保持按下
                if (pressed)
                {
                    // 持续计时，统计按下时长
                    keyFsm[k].cnt++;

                    // 当按下时长达到长按阈值，认为触发长按
                    if (keyFsm[k].cnt >= KEY_FSM_LONG_PRESS_CNT)
                    {
                        // 切换到长按状态
                        keyFsm[k].state = KEY_STATE_LONGPRESS;
                        // 清零计数器
                        keyFsm[k].cnt   = 0;
                        // 产生一次长按事件
                        keyFsm[k].event = KEY_EVENT_LONG_PRESS;
                    }
                }
                // 按键在达到长按阈值之前释放 -> 判定为一次短按
                else
                {
                    // 回到空闲状态
                    keyFsm[k].state = KEY_STATE_IDLE;
                    // 在释放时刻产生短按事件，而不是在按下时刻
                    keyFsm[k].event = KEY_EVENT_PRESS;
                }
                break;

            // ----------------------------
            // 状态 3：LONGPRESS（长按保持）
            // ----------------------------
            case KEY_STATE_LONGPRESS:
                // 长按期间持续检测按键是否释放
                if (!pressed)
                {
                    // 按键释放，回到空闲状态
                    keyFsm[k].state = KEY_STATE_IDLE;
                    // 产生一次释放事件（长按释放）
                    keyFsm[k].event = KEY_EVENT_RELEASE;
                }
                // 若仍保持按下，继续停留在 LONGPRESS，不产生新事件
                break;

            // ----------------------------
            // 异常状态保护
            // ----------------------------
            default:
                // 若状态变量被意外篡改，强制恢复为 IDLE
                keyFsm[k].state = KEY_STATE_IDLE;
                break;
        }
    }
}

// 读取消抖后的稳定按键状态：1 = 按下，0 = 松开
// 直接查状态机：只有确认按下(PRESSED)或长按保持(LONGPRESS)才算按下
uint8_t Key_IsDown(uint8_t key)
{
    if (key < KEY_A || key > KEY_RG)
    {
        return 0U;
    }

    if ((keyFsm[key].state == KEY_STATE_PRESSED) ||
        (keyFsm[key].state == KEY_STATE_LONGPRESS))
    {
        return 1U;
    }
    return 0U;
}

// 读取指定按键的当前事件，读取后自动清除
uint8_t Key_FSM_GetEvent(uint8_t key)
{
    // 参数合法性检查：key 编号必须在 KEY_A ~ KEY_RG 之间
    if (key < KEY_A || key > KEY_RG)
        return KEY_EVENT_NONE;

    // 第1步：取出当前事件
    uint8_t evt = keyFsm[key].event;

    // 第2步：清空事件，防止重复读取同一事件
    keyFsm[key].event = KEY_EVENT_NONE;

    // 第3步：返回事件给调用者
    return evt;
}
