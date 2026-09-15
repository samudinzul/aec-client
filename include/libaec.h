/* Generated with cbindgen:0.26.0 */

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>

// Add this line: forward declaration of the opaque Aec type
typedef struct Aec Aec;

extern "C" {

Aec *AecNew(uintptr_t frame_size,
            int32_t filter_length,
            uint32_t sample_rate,
            bool enable_preprocess);

void AecCancelEcho(Aec *aec_ptr,
                   const int16_t *rec_buffer,
                   const int16_t *echo_buffer,
                   int16_t *out_buffer,
                   uintptr_t buffer_length);

void AecDestroy(Aec *aec_ptr);

} // extern "C"