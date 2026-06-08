################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
S_SRCS += \
../Application/Startup/startup_stm32mp157caax.s 

OBJS += \
./Application/Startup/startup_stm32mp157caax.o 

S_DEPS += \
./Application/Startup/startup_stm32mp157caax.d 


# Each subdirectory must supply rules for building sources it contributes
Application/Startup/startup_stm32mp157caax.o: C:/Users/Admin/Desktop/stm_m4/STM32CubeMP1/Projects/STM32MP157C-DK2/Examples/GPIO/GPIO_EXTI/STM32CubeIDE/CM4/Application/Startup/startup_stm32mp157caax.s Application/Startup/subdir.mk
	arm-none-eabi-gcc -mcpu=cortex-m4 -g3 -c -x assembler-with-cpp -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@" "$<"

clean: clean-Application-2f-Startup

clean-Application-2f-Startup:
	-$(RM) ./Application/Startup/startup_stm32mp157caax.d ./Application/Startup/startup_stm32mp157caax.o

.PHONY: clean-Application-2f-Startup

