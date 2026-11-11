package dependency;

@java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
@java.lang.annotation.Target(java.lang.annotation.ElementType.TYPE)
public @interface Note {
    String value();
    String[] names() default {"left", "right"};
}
