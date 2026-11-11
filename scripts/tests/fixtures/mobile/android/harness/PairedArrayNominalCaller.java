import fixture.*;
public final class PairedArrayNominalCaller {
    public static Object call(Object value, Property property, Evaluator evaluator, Object[] values, int shape) {
        Result actual = Provider.make(value, property, evaluator, values);
        return shape == 0 ? actual : null;
    }
}
