################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/system_stm32mp1xx.c 

OBJS += \
./Drivers/CMSIS/system_stm32mp1xx.o 

C_DEPS += \
./Drivers/CMSIS/system_stm32mp1xx.d 


# Each subdirectory must supply rules for building sources it contributes
Drivers/CMSIS/system_stm32mp1xx.o: C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/system_stm32mp1xx.c Drivers/CMSIS/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_HAL_DRIVER -DSTM32MP157Cxx -DCORE_CM4 -DDEBUG -c -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc -I../../../../../../../../Drivers/CMSIS/Device/ST/STM32MP1xx/Include -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc/Legacy -I../../../../../../../../Drivers/BSP/STM32MP15xx_DISCO -I../../../Inc -I../../../../../../../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-CMSIS

clean-Drivers-2f-CMSIS:
	-$(RM) ./Drivers/CMSIS/system_stm32mp1xx.cyclo ./Drivers/CMSIS/system_stm32mp1xx.d ./Drivers/CMSIS/system_stm32mp1xx.o ./Drivers/CMSIS/system_stm32mp1xx.su

.PHONY: clean-Drivers-2f-CMSIS

