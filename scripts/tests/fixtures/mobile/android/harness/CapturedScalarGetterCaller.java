import fixture.Client;
import fixture.Owner;
public final class CapturedScalarGetterCaller {
    public static Object call(int route, Owner.Cursor cursor, int index) {
        switch (route) {
        case 0: return Client.direct(cursor, index);
        case 1: return Client.via(cursor, index);
        case 2: return Client.cast(cursor, index);
        case 3: return Client.viaCast(cursor, index);
        case 4: return Client.append(cursor, index);
        case 5: return Client.viaAppend(cursor, index);
        default: throw new AssertionError("invalid owned route");
        }
    }
}
