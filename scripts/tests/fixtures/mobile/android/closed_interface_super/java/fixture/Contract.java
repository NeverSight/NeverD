package fixture;
public class Contract {
    public interface Api {
        default Result choose(Item item) {
            Trace.step(1);
            Result result = Trace.fallback;
            Trace.receiver = this; Trace.item = item;
            Trace.step(2);
            return result;
        }
    }
}
