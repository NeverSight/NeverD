package fixture;
import java.util.Set;
public interface Factory<T> { T create(Set<Class<?>> seen); }
