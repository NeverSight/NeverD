import fixture.*;
public class AnchoredVoidCarrierCaller {
    public static void call(Cell<Object> cell, Callback<Object,Boolean> callback,
                            Callback<String,Boolean> fixed, int value) {
        Core.call(cell, callback, fixed, value);
    }
}
