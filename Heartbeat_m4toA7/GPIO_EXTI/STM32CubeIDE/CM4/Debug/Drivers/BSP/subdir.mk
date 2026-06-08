################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Drivers/BSP/STM32MP15xx_DISCO/stm32mp15xx_disco.c 

OBJS += \
./Drivers/BSP/stm32mp15xx_disco.o 

C_DEPS += \
./Drivers/BSP/stm32mp15xx_disco.d 


# Each subdirectory must supply rules for building sources it contributes
Drivers/BSP/stm32mp15xx_disco.o: C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Drivers/BSP/STM32MP15xx_DISCO/stm32mp15xx_disco.c Drivers/BSP/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_HAL_DRIVER -DSTM32MP157Cxx -DCORE_CM4 -DDEBUG -c -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc -I../../../../../../../../Drivers/CMSIS/Device/ST/STM32MP1xx/Include -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc/Legacy -I../../../../../../../../Drivers/BSP/STM32MP15xx_DISCO -I../../../Inc -I../../../../../../../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-BSP

clean-Drivers-2f-BSP:
	-$(RM) ./Drivers/BSP/stm32mp15xx_disco.cyclo ./Drivers/BSP/stm32mp15xx_disco.d ./Drivers/BSP/stm32mp15xx_disco.o ./Drivers/BSP/stm32mp15xx_disco.su

.PHONY: clean-Drivers-2f-BSP

