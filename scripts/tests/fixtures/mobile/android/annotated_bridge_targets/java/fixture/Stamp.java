package fixture;
import java.lang.annotation.*;
@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.METHOD)
public @interface Stamp { int value() default -1; String label() default "none"; }
