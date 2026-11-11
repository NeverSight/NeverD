package fixture;

import java.util.function.Predicate;

public class Provider {
    public Provider() {}
    @SuppressWarnings("unchecked")
    public <T> Provider bind(String key, Class<T> token, Predicate<T> predicate) {
        Trace.hit(1,this,key,token,predicate,null);
        Trace.accepted=predicate.test((T)Trace.value);
        Provider result=Trace.returned;
        Trace.hit(2,this,key,token,predicate,result);
        return result;
    }
}
