import fixture.Builder;
import fixture.Child;
import fixture.Path;
public final class ScopedGetterCaller {
    public static Child<Object,Object,Object> make(Builder<Object,Object> builder, Path<Object,Object,Object>[] path) {
        return new Child<Object,Object,Object>(builder, path);
    }
}
