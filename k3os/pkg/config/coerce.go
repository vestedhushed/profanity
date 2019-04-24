package config

import (
	"github.com/rancher/mapper"
	"github.com/rancher/mapper/mappers"
)

type Converter func(val interface{}) interface{}

type fieldConverter struct {
	mappers.DefaultMapper
	fieldName string
	converter Converter
}

func (f fieldConverter) ToInternal(data map[string]interface{}) error {
	val, ok := data[f.fieldName]
	if !ok {
		return nil
	}
	data[f.fieldName] = f.converter(val)
	return nil
}

type typeConverter struct {
	mappers.DefaultMapper
	converter Converter
	fieldType string
	mappers   mapper.Mappers
}

func (t *typeConverter) ModifySchema(schema *mapper.Schema, schemas *mapper.Schemas) error {
	for name, field := range schema.ResourceFields {
		if field.Type == t.fieldType {
			t.mappers = append(t.mappers, fieldConverter{
				fieldName: name,
				converter: t.converter,
			})
		}
	}
	return nil
}

func NewTypeConverter(fieldType string, converter Converter) mapper.Mapper {
	return &typeConverter{
		fieldType: fieldType,
		converter: converter,
	}
}

func NewToSlice() mapper.Mapper {
	return NewTypeConverter("array[string]", func(val interface{}) interface{} {
		if str, ok := val.(string); ok {
			return []string{str}
		}
		return val
	})
}

func NewToBool() mapper.Mapper {
	return NewTypeConverter("bool", func(val interface{}) interface{} {
		if str, ok := val.(string); ok {
			return str == "true"
		}
		return val
	})
}
