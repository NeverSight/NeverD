package fixture;
import fixture.api.Facade;
public final class Client {
    public static Receipt objects(Object[] values) {
        Receipt answer = Facade.collect(values);
        Trace.after = answer;
        return answer;
    }
    public static Receipt strings(String[] values) {
        Receipt answer = Facade.collect(values);
        Trace.after = answer;
        return answer;
    }
}
