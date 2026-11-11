import fixture.*;
public class ScopedRawConstructorsCaller {
    public static Core<String, Integer> create(Parent input) { return new Core<String, Integer>(input); }
}
