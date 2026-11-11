package fixture;
public final class Caller { private Caller() {} public static Object call(Core value) { return value.get(); }     public static byte[] read(Core value) { return value.readValue(); }
}
