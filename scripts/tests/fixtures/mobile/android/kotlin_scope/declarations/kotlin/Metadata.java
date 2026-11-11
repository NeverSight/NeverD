package kotlin;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;
import java.lang.annotation.ElementType;

// Owned test declaration for the metadata carrier, without a Kotlin runtime.
@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.TYPE)
public @interface Metadata {
    int k() default 1;
    int[] mv() default {};
    int[] bv() default {};
    String[] d1() default {};
    String[] d2() default {};
    String xs() default "";
    String pn() default "";
    int xi() default 0;
}
