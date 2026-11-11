import fixture.*;
public final class RawContextArrayNominalCaller {
    public static Object call(Property property, Evaluator evaluator, Object[] values, int shape) {
        Result actual = Provider.make(property, evaluator, values);
        return shape == 0 ? actual : null;
    }
}
