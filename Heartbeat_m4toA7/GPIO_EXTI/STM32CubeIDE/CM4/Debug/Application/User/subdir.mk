################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/lock_resource.c \
C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/main.c \
C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/stm32mp1xx_hal_msp.c \
C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/stm32mp1xx_it.c \
../Application/User/syscalls.c 

OBJS += \
./Application/User/lock_resource.o \
./Application/User/main.o \
./Application/User/stm32mp1xx_hal_msp.o \
./Application/User/stm32mp1xx_it.o \
./Application/User/syscalls.o 

C_DEPS += \
./Application/User/lock_resource.d \
./Application/User/main.d \
./Application/User/stm32mp1xx_hal_msp.d \
./Application/User/stm32mp1xx_it.d \
./Application/User/syscalls.d 


# Each subdirectory must supply rules for building sources it contributes
Application/User/lock_resource.o: C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/lock_resource.c Application/User/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_HAL_DRIVER -DSTM32MP157Cxx -DCORE_CM4 -DDEBUG -c -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc -I../../../../../../../../Drivers/CMSIS/Device/ST/STM32MP1xx/Include -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc/Legacy -I../../../../../../../../Drivers/BSP/STM32MP15xx_DISCO -I../../../Inc -I../../../../../../../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Application/User/main.o: C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/main.c Application/User/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_HAL_DRIVER -DSTM32MP157Cxx -DCORE_CM4 -DDEBUG -c -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc -I../../../../../../../../Drivers/CMSIS/Device/ST/STM32MP1xx/Include -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc/Legacy -I../../../../../../../../Drivers/BSP/STM32MP15xx_DISCO -I../../../Inc -I../../../../../../../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Application/User/stm32mp1xx_hal_msp.o: C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/stm32mp1xx_hal_msp.c Application/User/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_HAL_DRIVER -DSTM32MP157Cxx -DCORE_CM4 -DDEBUG -c -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc -I../../../../../../../../Drivers/CMSIS/Device/ST/STM32MP1xx/Include -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc/Legacy -I../../../../../../../../Drivers/BSP/STM32MP15xx_DISCO -I../../../Inc -I../../../../../../../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Application/User/stm32mp1xx_it.o: C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/Src/stm32mp1xx_it.c Application/User/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_HAL_DRIVER -DSTM32MP157Cxx -DCORE_CM4 -DDEBUG -c -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc -I../../../../../../../../Drivers/CMSIS/Device/ST/STM32MP1xx/Include -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc/Legacy -I../../../../../../../../Drivers/BSP/STM32MP15xx_DISCO -I../../../Inc -I../../../../../../../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"
Application/User/%.o Application/User/%.su Application/User/%.cyclo: ../Application/User/%.c Application/User/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_HAL_DRIVER -DSTM32MP157Cxx -DCORE_CM4 -DDEBUG -c -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc -I../../../../../../../../Drivers/CMSIS/Device/ST/STM32MP1xx/Include -I../../../../../../../../Drivers/STM32MP1xx_HAL_Driver/Inc/Legacy -I../../../../../../../../Drivers/BSP/STM32MP15xx_DISCO -I../../../Inc -I../../../../../../../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Application-2f-User

clean-Application-2f-User:
	-$(RM) ./Application/User/lock_resource.cyclo ./Application/User/lock_resource.d ./Application/User/lock_resource.o ./Application/User/lock_resource.su ./Application/User/main.cyclo ./Application/User/main.d ./Application/User/main.o ./Application/User/main.su ./Application/User/stm32mp1xx_hal_msp.cyclo ./Application/User/stm32mp1xx_hal_msp.d ./Application/User/stm32mp1xx_hal_msp.o ./Application/User/stm32mp1xx_hal_msp.su ./Application/User/stm32mp1xx_it.cyclo ./Application/User/stm32mp1xx_it.d ./Application/User/stm32mp1xx_it.o ./Application/User/stm32mp1xx_it.su ./Application/User/syscalls.cyclo ./Application/User/syscalls.d ./Application/User/syscalls.o ./Application/User/syscalls.su

.PHONY: clean-Application-2f-User

