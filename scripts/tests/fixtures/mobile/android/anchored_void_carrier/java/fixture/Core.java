package fixture;
public class Core {
    public static void call(Cell<Object> cell, Callback<Object,Boolean> callback,
                            Callback<String,Boolean> fixed, int value) {
        Provider.send(cell, callback, fixed, value);
    }
}
