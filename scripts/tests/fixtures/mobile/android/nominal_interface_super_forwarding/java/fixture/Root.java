package fixture;
public interface Root extends Leaf {
    default Output convert(Input value) {
        Trace.begin(this, value);
        Output result = new Output(transform(value.read()));
        Trace.after(result);
        return result;
    }
}
