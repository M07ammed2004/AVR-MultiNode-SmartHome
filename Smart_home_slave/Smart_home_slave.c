#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>

#define F_CPU 8000000UL
#define BAUD 9600
#define MYUBRR (F_CPU/16/BAUD-1)

// Stepper motor connections (verified)
#define STEPPER_PORT PORTB
#define STEPPER_DDR DDRB
#define IN1 PB3
#define IN2 PB4
#define IN3 PB5
#define IN4 PB6

// DHT11 sensor configuration with improved timing
#define DHT11_PIN PD2
#define DHT11_DDR DDRD
#define DHT11_PORT PORTD
#define DHT11_PIN_REG PIND

// System configuration
#define STEPS_PER_FLOOR 512    // Adjust based on mechanical requirements
#define GROUND_FLOOR 0
#define FIRST_FLOOR 1

// Corrected Ground floor components (verified pin assignments)
#define SERVO_PIN PB1         // Door servo control
#define FAN_PIN1 PC0          // Fan control pin 1
#define FAN_PIN2 PC1          // Fan control pin 2
#define FAN_ENABLE PC2        // Fan enable
#define LED_RED PD3           // RGB LED pins moved to PORTD
#define LED_GREEN PD4
#define LED_BLUE PD5
#define FLOOR_LED PC6         // Ground floor indicator
#define FIRST_FLOOR_LED PC7   // First floor indicator
#define BUZZER_PIN PB0        // Buzzer for feedback

// First floor components (verified)
#define FIRST_FAN_PIN1 PA2
#define FIRST_FAN_PIN2 PA3
#define FIRST_FAN_ENABLE PA4
#define FIRST_LED_RED PA5
#define FIRST_LED_GREEN PA6
#define FIRST_LED_BLUE PA7

volatile uint8_t current_floor = GROUND_FLOOR;
volatile int temperature = 0;
volatile int humidity = 0;
volatile uint8_t command[3];
volatile uint8_t byte_count = 0;

/* USART Initialization with interrupts */
void USART_Init(unsigned int ubrr) {
    UBRRH = (unsigned char)(ubrr>>8);
    UBRRL = (unsigned char)ubrr;
    UCSRB = (1<<RXEN)|(1<<TXEN)|(1<<RXCIE); // Enable RX/TX and RX complete interrupt
    UCSRC = (1<<URSEL)|(1<<USBS)|(3<<UCSZ0); // 8-bit data, 1 stop bit
    sei(); // Enable global interrupts
}

/* USART Transmit with timeout protection */
void USART_Transmit(unsigned char data) {
    unsigned int timeout = 0;
    while (!(UCSRA & (1 << UDRE))) {
        if(timeout++ > 10000) return; // Timeout after ~100ms
        _delay_us(10);
    }
    UDR = data;
}

/* Buzzer control with safety limits */
void buzzer_beep(uint16_t duration_ms) {
    if(duration_ms > 2000) duration_ms = 2000;
    PORTB |= (1<<BUZZER_PIN);
    _delay_ms(duration_ms);
    PORTB &= ~(1<<BUZZER_PIN);
}

/* Enhanced DHT11 reading function with robust error handling */
uint8_t dht11_read() {
    uint8_t bits[5] = {0};
    uint8_t i, j;
    uint16_t timeout;

    // Send start signal
    DHT11_DDR |= (1<<DHT11_PIN);  // Set as output
    DHT11_PORT &= ~(1<<DHT11_PIN); // Pull low
    _delay_ms(20);                 // 20ms minimum per datasheet

    DHT11_PORT |= (1<<DHT11_PIN);  // Pull high
    _delay_us(30);                 // 30us high

    // Set as input with pull-up
    DHT11_DDR &= ~(1<<DHT11_PIN);
    DHT11_PORT |= (1<<DHT11_PIN);
    _delay_us(40);

    // Wait for sensor response (low)
    timeout = 1000;
    while(DHT11_PIN_REG & (1<<DHT11_PIN) && --timeout)
        _delay_us(1);
    if(timeout == 0) return 0;

    // Wait for sensor response (high)
    timeout = 1000;
    while(!(DHT11_PIN_REG & (1<<DHT11_PIN)) && --timeout)
        _delay_us(1);
    if(timeout == 0) return 0;

    // Read 40 bits of data
    for(j=0; j<5; j++) {
        for(i=0; i<8; i++) {
            // Wait for start of bit (low)
            timeout = 1000;
            while(!(DHT11_PIN_REG & (1<<DHT11_PIN)) && --timeout)
                _delay_us(1);
            if(timeout == 0) return 0;

            _delay_us(35); // Critical timing threshold between 0 and 1

            if(DHT11_PIN_REG & (1<<DHT11_PIN)) {
                bits[j] |= (1<<(7-i)); // It's a 1
                timeout = 1000;
                while(DHT11_PIN_REG & (1<<DHT11_PIN) && --timeout)
                    _delay_us(1);
                if(timeout == 0) return 0;
            }
        }
    }

    // Verify checksum
    if(bits[4] != ((bits[0] + bits[1] + bits[2] + bits[3]) & 0xFF))
        return 0;

    humidity = bits[0];
    temperature = bits[2];
    return 1;
}

/* Initialize all hardware components */
void init_components() {
    // Stepper motor initialization
    STEPPER_DDR |= (1<<IN1)|(1<<IN2)|(1<<IN3)|(1<<IN4);
    STEPPER_PORT &= ~((1<<IN1)|(1<<IN2)|(1<<IN3)|(1<<IN4));

    // DHT11 sensor initialization
    DHT11_DDR |= (1<<DHT11_PIN);
    DHT11_PORT |= (1<<DHT11_PIN);

    // Servo and buzzer setup
    DDRB |= (1<<SERVO_PIN)|(1<<BUZZER_PIN);
    // Configure Timer1 for servo control (50Hz PWM)
    TCCR1A = (1<<COM1A1)|(1<<WGM11);
    TCCR1B = (1<<WGM13)|(1<<WGM12)|(1<<CS11);
    ICR1 = 19999;    // 50Hz frequency (20ms period)
    OCR1A = 1500;    // Neutral position (90°)

    // Ground floor components initialization
    DDRC |= (1<<FAN_PIN1)|(1<<FAN_PIN2)|(1<<FAN_ENABLE);
    DDRD |= (1<<LED_RED)|(1<<LED_GREEN)|(1<<LED_BLUE); // RGB LEDs on PORTD
    DDRC |= (1<<FLOOR_LED)|(1<<FIRST_FLOOR_LED);

    // First floor components initialization
    DDRA |= (1<<FIRST_FAN_PIN1)|(1<<FIRST_FAN_PIN2)|(1<<FIRST_FAN_ENABLE);
    DDRA |= (1<<FIRST_LED_RED)|(1<<FIRST_LED_GREEN)|(1<<FIRST_LED_BLUE);

    // Initial state
    current_floor = GROUND_FLOOR;
    PORTC |= (1<<FLOOR_LED);       // Ground floor LED on
    PORTC &= ~(1<<FIRST_FLOOR_LED); // First floor LED off
    buzzer_beep(200);              // Startup confirmation beep
}

/* Servo control with position limits */
void set_servo_position(uint16_t pos) {
    if(pos < 1000) pos = 1000;  // Minimum pulse width (0°)
    if(pos > 2000) pos = 2000;  // Maximum pulse width (180°)
    OCR1A = pos;
    _delay_ms(15);  // Allow time for movement
    buzzer_beep(50); // Audible feedback
}

/* Fan speed control for both floors */
void set_fan_speed(uint8_t floor, uint8_t speed) {
    if(floor == GROUND_FLOOR) {
        PORTC &= ~((1<<FAN_PIN1)|(1<<FAN_PIN2)|(1<<FAN_ENABLE));

        switch(speed) {
            case '1': // Low speed
                PORTC |= (1<<FAN_PIN1)|(1<<FAN_ENABLE);
                break;
            case '2': // High speed
                PORTC |= (1<<FAN_PIN2)|(1<<FAN_ENABLE);
                break;
            default: // Off
                break;
        }
    } else {
        PORTA &= ~((1<<FIRST_FAN_PIN1)|(1<<FIRST_FAN_PIN2)|(1<<FIRST_FAN_ENABLE));

        switch(speed) {
            case '1': // Low speed
                PORTA |= (1<<FIRST_FAN_PIN1)|(1<<FIRST_FAN_ENABLE);
                break;
            case '2': // High speed
                PORTA |= (1<<FIRST_FAN_PIN2)|(1<<FIRST_FAN_ENABLE);
                break;
            default: // Off
                break;
        }
    }
    buzzer_beep(100); // Operation feedback
}

/* LED color control for both floors - fixed ground floor LED flickering */
void set_led_color(uint8_t floor, char color) {
    if(floor == GROUND_FLOOR) {
        // Atomic operation to prevent flickering
        uint8_t current = PORTD & ~((1<<LED_RED)|(1<<LED_GREEN)|(1<<LED_BLUE));

        switch(color) {
            case 'R': current |= (1<<LED_RED); break;
            case 'G': current |= (1<<LED_GREEN); break;
            case 'B': current |= (1<<LED_BLUE); break;
            case 'W': current |= (1<<LED_RED)|(1<<LED_GREEN)|(1<<LED_BLUE); break;
            default: break; // Off
        }
        PORTD = current; // Single write to PORTD
    } else {
        PORTA &= ~((1<<FIRST_LED_RED)|(1<<FIRST_LED_GREEN)|(1<<FIRST_LED_BLUE));

        switch(color) {
            case 'R': PORTA |= (1<<FIRST_LED_RED); break;
            case 'G': PORTA |= (1<<FIRST_LED_GREEN); break;
            case 'B': PORTA |= (1<<FIRST_LED_BLUE); break;
            case 'W': PORTA |= (1<<FIRST_LED_RED)|(1<<FIRST_LED_GREEN)|(1<<FIRST_LED_BLUE); break;
            default: break; // Off
        }
    }
    buzzer_beep(50); // Operation feedback
}

/* Stepper motor sequence (half-step mode for smoother movement) */
const uint8_t stepper_seq[8] = {
    (1<<IN1),
    (1<<IN1)|(1<<IN2),
    (1<<IN2),
    (1<<IN2)|(1<<IN3),
    (1<<IN3),
    (1<<IN3)|(1<<IN4),
    (1<<IN4),
    (1<<IN4)|(1<<IN1)
};

/* Move stepper motor one step */
void move_stepper(uint8_t dir) {
    static uint8_t step = 0;

    if(dir) step = (step + 1) % 8; // Clockwise
    else step = (step == 0) ? 7 : step - 1; // Counter-clockwise

    STEPPER_PORT = (STEPPER_PORT & ~((1<<IN1)|(1<<IN2)|(1<<IN3)|(1<<IN4))) | stepper_seq[step];
    _delay_ms(5); // Adjust for desired motor speed
}

/* Move elevator to target floor */
void move_to_floor(uint8_t target) {
    if(current_floor == target) return;

    // Visual and audible movement start indication
    PORTC &= ~((1<<FLOOR_LED)|(1<<FIRST_FLOOR_LED));
    buzzer_beep(300);

    uint16_t steps = STEPS_PER_FLOOR;
    uint8_t direction = (target == FIRST_FLOOR) ? 1 : 0;

    while(steps--) {
        move_stepper(direction);
        _delay_ms(10); // Adjust for desired elevator speed
    }

    // Update floor and indicators
    current_floor = target;
    if(target == FIRST_FLOOR) {
        PORTC |= (1<<FIRST_FLOOR_LED);
    } else {
        PORTC |= (1<<FLOOR_LED);
    }

    // Turn off motor coils to save power
    STEPPER_PORT &= ~((1<<IN1)|(1<<IN2)|(1<<IN3)|(1<<IN4));
    buzzer_beep(200); // Movement complete
}

/* USART receive interrupt handler */
ISR(USART_RXC_vect) {
    command[byte_count++] = UDR;

    if(byte_count == 3) { // Complete command received
        switch(command[0]) {
            case 'D': // Door control
                if(command[1] == 'O') set_servo_position(1500); // Open
                else if(command[1] == 'C') set_servo_position(1000); // Close
                break;

            case 'G': // Ground floor control
                if(command[1] == 'F') set_fan_speed(GROUND_FLOOR, command[2]);
                else if(command[1] == 'L') set_led_color(GROUND_FLOOR, command[2]);
                else if(command[1] == 'I') { // Indicator
                    PORTC &= ~((1<<FLOOR_LED)|(1<<FIRST_FLOOR_LED));
                    if(command[2] == '1') PORTC |= (1<<FLOOR_LED);
                }
                break;

            case 'F': // First floor control
                if(command[1] == 'F') set_fan_speed(FIRST_FLOOR, command[2]);
                else if(command[1] == 'L') set_led_color(FIRST_FLOOR, command[2]);
                else if(command[1] == 'I') { // Indicator
                    PORTC &= ~((1<<FLOOR_LED)|(1<<FIRST_FLOOR_LED));
                    if(command[2] == '1') PORTC |= (1<<FIRST_FLOOR_LED);
                }
                break;

            case 'E': // Elevator control
                if(command[1] == 'U') move_to_floor(FIRST_FLOOR);
                else if(command[1] == 'D') move_to_floor(GROUND_FLOOR);
                break;

            case 'S': // Sensor reading
                if(command[1] == 'R') {
                    if(dht11_read()) {
                        USART_Transmit(temperature);
                        _delay_ms(10);
                        USART_Transmit(humidity);
                    } else {
                        USART_Transmit(0xFF); // Error code
                        _delay_ms(10);
                        USART_Transmit(0xFF);
                    }
                }
                break;
        }
        byte_count = 0; // Reset for next command
    }
}

int main(void) {
    USART_Init(MYUBRR);
    init_components();

    while(1) {
        _delay_ms(100);
    }
}
