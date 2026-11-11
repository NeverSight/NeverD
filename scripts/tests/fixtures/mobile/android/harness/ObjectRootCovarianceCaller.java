import fixture.*;
public final class ObjectRootCovarianceCaller {
    public static Core<String> copy(Core<String> self) throws CloneNotSupportedException {
        return self.clone();
    }
}
