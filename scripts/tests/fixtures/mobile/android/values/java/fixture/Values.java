package fixture;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.TYPE, ElementType.FIELD, ElementType.METHOD,
         ElementType.PARAMETER, ElementType.CONSTRUCTOR})
public @interface Values {
    String[] data() default {"first", "\u0000\n\"\\", ""};
    int[] version() default {2, 2, 0};
    int number() default -7;
    boolean active() default true;
    long wide() default -9223372036854775808L;
    float negativeZero() default -0.0f;
    double fraction() default 0.125;
    NestedValue nested() default @NestedValue(name = "inside");
}
