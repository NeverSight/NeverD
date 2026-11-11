package fixture;
public final class Client {
    public static Object direct(Owner.Cursor cursor, int index) {
        Object value = cursor.get(index); Trace.hit(7); return value;
    }
    public static Object via(Api cursor, int index) {
        Object value = cursor.get(index); Trace.hit(7); return value;
    }
    public static String cast(Owner.Cursor cursor, int index) {
        Object value = cursor.get(index); Trace.hit(7); return (String) value;
    }
    public static String viaCast(Api cursor, int index) {
        Object value = cursor.get(index); Trace.hit(7); return (String) value;
    }
    public static String append(Owner.Cursor cursor, int index) {
        Object value = cursor.get(index); Trace.hit(7);
        return new StringBuilder().append(value).toString();
    }
    public static String viaAppend(Api cursor, int index) {
        Object value = cursor.get(index); Trace.hit(7);
        return new StringBuilder().append(value).toString();
    }
}
