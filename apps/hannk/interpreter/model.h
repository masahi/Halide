#ifndef MODEL_H
#define MODEL_H

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "HalideBuffer.h"
#include "interval.h"
#include "util/error_util.h"

namespace hannk {

template<class T, class... Args>
std::unique_ptr<T> make_unique(Args &&...args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

template<typename T>
using HalideBuffer = Halide::Runtime::Buffer<T>;

enum class TensorType {
    // Note that these are deliberately ordered and valued to match tflite's
    // similar enum; there is no reason these types *must* have the same values,
    // but as the values arbitrary otherwise, we might as well match.
    Float32 = 0,
    Float16 = 1,
    Int32 = 2,
    UInt8 = 3,
    Int64 = 4,
    String = 5,
    Bool = 6,
    Int16 = 7,
    Complex64 = 8,
    Int8 = 9,
    Float64 = 10,
    Complex128 = 11,
};

size_t sizeof_tensor_type(TensorType t);
const char *to_string(TensorType t);
halide_type_t to_halide_type(TensorType t);

template<typename T>
inline TensorType to_tensor_type() {
    if (std::is_const<T>::value) {
        return to_tensor_type<typename std::remove_const<T>::type>();
    }
    CHECK(0) << "Type is not convertible to TensorType";
    // unreachable
}

template<>
inline TensorType to_tensor_type<float>() {
    return TensorType::Float32;
}
template<>
inline TensorType to_tensor_type<int32_t>() {
    return TensorType::Int32;
}
template<>
inline TensorType to_tensor_type<uint8_t>() {
    return TensorType::UInt8;
}
template<>
inline TensorType to_tensor_type<int64_t>() {
    return TensorType::Int64;
}
template<>
inline TensorType to_tensor_type<std::string>() {
    return TensorType::String;
}
template<>
inline TensorType to_tensor_type<bool>() {
    return TensorType::Bool;
}
template<>
inline TensorType to_tensor_type<int16_t>() {
    return TensorType::Int16;
}
template<>
inline TensorType to_tensor_type<int8_t>() {
    return TensorType::Int8;
}
template<>
inline TensorType to_tensor_type<double>() {
    return TensorType::Float64;
}
// TODO
//template<> inline TensorType to_tensor_type<float16>() { return TensorType::Float16; }

template<typename T>
inline bool is_type(TensorType t) {
    return t == to_tensor_type<T>();
}

struct QuantizationInfo {
    std::vector<float> scale;
    std::vector<int32_t> zero;
    int32_t dimension = -1;
};

inline std::ostream &operator<<(std::ostream &s, const QuantizationInfo &q) {
    return s << "{" << q.scale << ", " << q.zero << ", " << q.dimension << "}";
}

class Tensor {
    std::string name_;
    TensorType type_;
    HalideBuffer<void> buffer_;
    QuantizationInfo quantization_;
    bool is_constant_ = false;
    bool is_input_ = false;
    bool is_output_ = false;

public:
    Tensor(std::string name, TensorType type, HalideBuffer<void> buffer, QuantizationInfo quantization)
        : name_(std::move(name)),
          type_(type),
          buffer_(std::move(buffer)),
          quantization_(std::move(quantization)) {
        is_constant_ = buffer_.data() != nullptr;
    }

    Tensor(const Tensor &copy) = default;

    hannk::TensorType type() const {
        return type_;
    }

    const std::string &name() const {
        return name_;
    }

    // TODO: not a good name. Maybe bounds()?
    Box box() const {
        const int dimensions = buffer_.dimensions();

        Box result;
        result.reserve(dimensions);
        for (int d = 0; d < dimensions; d++) {
            const auto dim = buffer_.dim(d);
            result.emplace_back(dim.min(), dim.max());
        }
        return result;
    }

    // TODO: not a good name.
    Interval interval(int i) const {
        const auto &d = buffer_.dim(i);
        return Interval(d.min(), d.max());
    }

    int extent(int i) const {
        return buffer_.dim(i).extent();
    }

    int rank() const {
        return buffer_.dimensions();
    }

    const QuantizationInfo &quantization() const {
        return quantization_;
    }

    bool is_constant() const {
        return is_constant_;
    }

    bool is_input() const {
        return is_input_;
    }

    bool is_output() const {
        return is_output_;
    }

    void set_input(bool is_input) {
        is_input_ = is_input;
    }

    void set_output(bool is_output) {
        is_output_ = is_output;
    }

    template<class T = void>
    const HalideBuffer<T> &buffer() {
        return buffer_.as<T>();
    }

    template<class T = void>
    const HalideBuffer<const T> &buffer() const {
        return buffer_.as_const().as<const T>();
    }

    template<class T = void>
    HalideBuffer<T> buffer(const Box &crop) {
        HalideBuffer<T> buf = buffer_.as<T>();
        for (int i = 0; i < (int)crop.size(); i++) {
            buf.crop(i, crop[i].min, crop[i].extent());
        }
        return buf;
    }

    template<class T = void>
    HalideBuffer<const T> buffer(const Box &crop) const {
        HalideBuffer<const T> buf = buffer_.as<const T>();
        for (int i = 0; i < (int)crop.size(); i++) {
            buf.crop(i, crop[i].min, crop[i].extent());
        }
        return buf;
    }

    bool is_allocated() const {
        return buffer_.data() != nullptr;
    }

    void allocate() {
        if (!buffer_.data()) {
            buffer_ = HalideBuffer<void>::make_with_shape_of(buffer_);
        }
    }

    void dump(std::ostream &os) const;

    Tensor() = delete;
    Tensor &operator=(const Tensor &) = delete;
    Tensor(Tensor &&) = default;
    Tensor &operator=(Tensor &&) = default;
};

// A mapping from old tensors to new tensors, when cloning an op.
using TensorMap = std::map<const Tensor *, Tensor *>;

// Apply a tensor map to a list of tensors. This is used to support
// cloning ops referring to different tensors.
Tensor *apply(const TensorMap &map, const Tensor *t);

// Required properties of a crop in a particular dimension.
struct SplitInfo {
    // Required alignment of crops in this dimension.
    int alignment;

    // Minimum extent of crops in this dimension.
    int min;

    // The default is not allowing any splits.
    SplitInfo()
        : SplitInfo(0, 1) {
    }
    SplitInfo(int alignment, int min)
        : alignment(alignment), min(min) {
    }

    static SplitInfo no_split() {
        return SplitInfo(0, 1);
    }
    static SplitInfo any_split() {
        return SplitInfo(1, 1);
    }
    static SplitInfo guard_with_if(int factor) {
        return SplitInfo(factor, 1);
    }
    static SplitInfo shift_inwards(int factor) {
        return SplitInfo(1, factor);
    }
    static SplitInfo round_up(int factor) {
        return SplitInfo(factor, factor);
    }
};

class Op {
protected:
    std::vector<Tensor *> inputs_;
    std::vector<Tensor *> outputs_;

    explicit Op(std::vector<Tensor *> inputs, std::vector<Tensor *> outputs)
        : inputs_(std::move(inputs)), outputs_(std::move(outputs)) {
    }

public:
    virtual ~Op() = default;

    // Get the shape of the complete output of this op.
    virtual Box get_full_crop() {
        if (output_count() == 1) {
            return output(0)->box();
        } else {
            CHECK(0) << "More than one output requires get_full_crop override.";
            return Box();
        }
    }

    // Get the bounds required of all inputs and outputs given a crop.
    struct Bounds {
        std::vector<Box> inputs;
        std::vector<Box> outputs;
    };
    virtual Bounds infer_bounds(const Box &crop) const = 0;

    // Execute the op on a given crop.
    virtual void execute(const Box &crop) = 0;

    // Get information about how crops of this op can be split.
    virtual std::vector<SplitInfo> get_split_info() const {
        return {};
    }

    // Clone this op, replacing tensors using the mapping in tensor_map.
    virtual std::unique_ptr<Op> clone(const TensorMap &tensor_map) const = 0;

    virtual void dump(std::ostream &os) const = 0;

    int input_count() const {
        return inputs_.size();
    }
    int output_count() const {
        return outputs_.size();
    }
    const Tensor *input(int idx) const {
        return inputs_[idx];
    }
    const Tensor *output(int idx) const {
        return outputs_[idx];
    }
    const Tensor *input() const {
        return input(0);
    }
    const Tensor *output() const {
        return output(0);
    }
    Tensor *input(int idx) {
        return inputs_[idx];
    }
    Tensor *output(int idx) {
        return outputs_[idx];
    }
    Tensor *input() {
        return input(0);
    }
    Tensor *output() {
        return output(0);
    }

    // Movable but not copyable.
    Op() = delete;
    Op(const Op &) = delete;
    Op &operator=(const Op &) = delete;
    Op(Op &&) = default;
    Op &operator=(Op &&) = default;
};

struct Model {
    std::vector<std::shared_ptr<Tensor>> tensors;
    std::vector<std::unique_ptr<Op>> ops;

    void dump(std::ostream &os);

    // Models can be copied. Tensors that are allocated will be
    // shared, tensors that are not allocated will be cloned.
    Model(const Model &);
    Model() = default;
    Model(Model &&) = default;
    Model &operator=(Model &&) = default;

    Model &operator=(const Model &) = delete;
};

}  // namespace hannk

#endif  // MODEL_H