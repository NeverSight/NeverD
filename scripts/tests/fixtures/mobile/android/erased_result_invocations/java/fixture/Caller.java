package fixture;
public final class Caller {
    public static Value direct(Provider self, Object token, int index, long stamp, double fraction) {
        return self.read(token, index, stamp, fraction);
    }
    public static Value viaApi(Api self, Object token, int index, long stamp, double fraction) {
        return self.read(token, index, stamp, fraction);
    }
    public static Item checked(Provider self, Object token, int index, long stamp, double fraction) {
        return self.read(token, index, stamp, fraction);
    }
    public static void ignored(Provider self, Object token, int index, long stamp, double fraction) {
        self.read(token, index, stamp, fraction);
    }
    public static Box stat(Object token, int index, long stamp, double fraction) {
        return Provider.staticRead(token, index, stamp, fraction);
    }
}
